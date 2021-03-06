/*
  * This program is free software: you can redistribute it and/or modify
  * it under the terms of the GNU General Public License as published by
  * the Free Software Foundation, either version 3 of the License, or
  * any later version.
  *
  * This program is distributed in the hope that it will be useful,
  * but WITHOUT ANY WARRANTY; without even the implied warranty of
  * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  * GNU General Public License for more details.
  *
  * You should have received a copy of the GNU General Public License
  * along with this program.  If not, see <http://www.gnu.org/licenses/>.
  *
  * Additional permission under GNU GPL version 3 section 7
  *
  * If you modify this Program, or any covered work, by linking or combining
  * it with OpenSSL (or a modified version of that library), containing parts
  * covered by the terms of OpenSSL License and SSLeay License, the licensors
  * of this Program grant you additional permission to convey the resulting work.
  *
  */

#include <assert.h>
#include <cmath>
#include <chrono>
#include <thread>
#include "console.h"

#ifdef _WIN32
#include <windows.h>

void thd_setaffinity(std::thread::native_handle_type h, uint64_t cpu_id)
{
	SetThreadAffinityMask(h, 1 << cpu_id);
}
#else
#include <pthread.h>

#if defined(__APPLE__)
#include <mach/thread_policy.h>
#include <mach/thread_act.h>
#define SYSCTL_CORE_COUNT   "machdep.cpu.core_count"
#endif

void thd_setaffinity(std::thread::native_handle_type h, uint64_t cpu_id)
{
#if defined(__APPLE__)
	thread_port_t mach_thread;
	thread_affinity_policy_data_t policy = { (int)cpu_id };
	mach_thread = pthread_mach_thread_np(h);
	thread_policy_set(mach_thread, THREAD_AFFINITY_POLICY, (thread_policy_t)&policy, 1);
#else
	cpu_set_t mn;
	CPU_ZERO(&mn);
	CPU_SET(cpu_id, &mn);
	pthread_setaffinity_np(h, sizeof(cpu_set_t), &mn);
#endif
}
#endif // _WIN32

#include "executor.h"
#include "minethd.h"
#include "jconf.h"
#include "crypto/cryptonight.h"

telemetry::telemetry(size_t iThd)
{
	ppHashCounts = new uint64_t*[iThd];
	ppTimestamps = new uint64_t*[iThd];
	iBucketTop = new uint32_t[iThd];

	for (size_t i = 0; i < iThd; i++)
	{
		ppHashCounts[i] = new uint64_t[iBucketSize];
		ppTimestamps[i] = new uint64_t[iBucketSize];
		iBucketTop[i] = 0;
		memset(ppHashCounts[0], 0, sizeof(uint64_t) * iBucketSize);
		memset(ppTimestamps[0], 0, sizeof(uint64_t) * iBucketSize);
	}
}

double telemetry::calc_telemetry_data(size_t iLastMilisec, size_t iThread)
{
	using namespace std::chrono;
	uint64_t iTimeNow = time_point_cast<milliseconds>(high_resolution_clock::now()).time_since_epoch().count();

	uint64_t iEarliestHashCnt = 0;
	uint64_t iEarliestStamp = 0;
	uint64_t iLastestStamp = 0;
	uint64_t iLastestHashCnt = 0;
	bool bHaveFullSet = false;

	//Start at 1, buckettop points to next empty
	for (size_t i = 1; i < iBucketSize; i++)
	{
		size_t idx = (iBucketTop[iThread] - i) & iBucketMask; //overflow expected here

		if (ppTimestamps[iThread][idx] == 0)
			break; //That means we don't have the data yet

		if (iLastestStamp == 0)
		{
			iLastestStamp = ppTimestamps[iThread][idx];
			iLastestHashCnt = ppHashCounts[iThread][idx];
		}

		if (iTimeNow - ppTimestamps[iThread][idx] > iLastMilisec)
		{
			bHaveFullSet = true;
			break; //We are out of the requested time period
		}

		iEarliestStamp = ppTimestamps[iThread][idx];
		iEarliestHashCnt = ppHashCounts[iThread][idx];
	}

	if (!bHaveFullSet || iEarliestStamp == 0 || iLastestStamp == 0)
		return nan("");

	//Don't think that can happen, but just in case
	if (iLastestStamp - iEarliestStamp == 0)
		return nan("");

	double fHashes, fTime;
	fHashes = iLastestHashCnt - iEarliestHashCnt;
	fTime = iLastestStamp - iEarliestStamp;
	fTime /= 1000.0;

	return fHashes / fTime;
}

void telemetry::push_perf_value(size_t iThd, uint64_t iHashCount, uint64_t iTimestamp)
{
	size_t iTop = iBucketTop[iThd];
	ppHashCounts[iThd][iTop] = iHashCount;
	ppTimestamps[iThd][iTop] = iTimestamp;

	iBucketTop[iThd] = (iTop + 1) & iBucketMask;
}

minethd::minethd(miner_work& pWork, size_t iNo, bool double_work, bool no_prefetch)
{
	oWork = pWork;
	bQuit = 0;
	iThreadNo = (uint8_t)iNo;
	iJobNo = 0;
	iHashCount = 0;
	iTimestamp = 0;
	bNoPrefetch = no_prefetch;

	if(double_work)
		oWorkThd = std::thread(&minethd::double_work_main, this);
	else
		oWorkThd = std::thread(&minethd::work_main, this);
}

std::atomic<uint64_t> minethd::iGlobalJobNo;
std::atomic<uint64_t> minethd::iConsumeCnt; //Threads get jobs as they are initialized
minethd::miner_work minethd::oGlobalWork;
uint64_t minethd::iThreadCount = 0;

cryptonight_ctx* minethd_alloc_ctx()
{
	cryptonight_ctx* ctx;
	alloc_msg msg = { 0 };

	switch (jconf::inst()->GetSlowMemSetting())
	{
	case jconf::never_use:
		ctx = cryptonight_alloc_ctx(1, 1, &msg);
		if (ctx == NULL)
			printer::inst()->print_msg(L0, "MEMORY ALLOC FAILED: %s", msg.warning);
		return ctx;

	case jconf::no_mlck:
		ctx = cryptonight_alloc_ctx(1, 0, &msg);
		if (ctx == NULL)
			printer::inst()->print_msg(L0, "MEMORY ALLOC FAILED: %s", msg.warning);
		return ctx;

	case jconf::print_warning:
		ctx = cryptonight_alloc_ctx(1, 1, &msg);
		if (msg.warning != NULL)
			printer::inst()->print_msg(L0, "MEMORY ALLOC FAILED: %s", msg.warning);
		if (ctx == NULL)
			ctx = cryptonight_alloc_ctx(0, 0, NULL);
		return ctx;

	case jconf::always_use:
		return cryptonight_alloc_ctx(0, 0, NULL);

	case jconf::unknown_value:
		return NULL; //Shut up compiler
	}

	return nullptr; //Should never happen
}

bool minethd::self_test()
{
	alloc_msg msg = { 0 };
	size_t res;
	bool fatal = false;

	switch (jconf::inst()->GetSlowMemSetting())
	{
	case jconf::never_use:
		res = cryptonight_init(1, 1, &msg);
		fatal = true;
		break;

	case jconf::no_mlck:
		res = cryptonight_init(1, 0, &msg);
		fatal = true;
		break;

	case jconf::print_warning:
		res = cryptonight_init(1, 1, &msg);
		break;

	case jconf::always_use:
		res = cryptonight_init(0, 0, &msg);
		break;

	case jconf::unknown_value:
	default:
		return false; //Shut up compiler
	}

	if(msg.warning != nullptr)
		printer::inst()->print_msg(L0, "MEMORY INIT ERROR: %s", msg.warning);

	if(res == 0 && fatal)
		return false;

	cryptonight_ctx *ctx0, *ctx1, *ctx2, *ctx3, *ctx4;
	if((ctx0 = minethd_alloc_ctx()) == nullptr)
		return false;

	if((ctx1 = minethd_alloc_ctx()) == nullptr)
	{
		cryptonight_free_ctx(ctx0);
		return false;
	}
	if((ctx2 = minethd_alloc_ctx()) == nullptr)
	{
		cryptonight_free_ctx(ctx0);
		cryptonight_free_ctx(ctx1);
		return false;
	}
	if((ctx3 = minethd_alloc_ctx()) == nullptr)
	{
		cryptonight_free_ctx(ctx0);
		cryptonight_free_ctx(ctx1);
		cryptonight_free_ctx(ctx2);
		return false;
	}
	if((ctx4 = minethd_alloc_ctx()) == nullptr)
	{
		cryptonight_free_ctx(ctx0);
		cryptonight_free_ctx(ctx1);
		cryptonight_free_ctx(ctx2);
		cryptonight_free_ctx(ctx3);
		return false;
	}

	bool bHasLp = ctx0->ctx_info[0] == 1 && ctx1->ctx_info[0] == 1;
	size_t n = jconf::inst()->GetThreadCount();
	jconf::thd_cfg cfg;
	for (size_t i = 0; i < n; i++)
	{
		jconf::inst()->GetThreadConfig(i, cfg);

		if(!bHasLp && cfg.bNoPrefetch)
		{
			printer::inst()->print_msg(L0, "Wrong config. You are running in slow memory mode with no_prefetch.");
			cryptonight_free_ctx(ctx0);
			cryptonight_free_ctx(ctx1);
			cryptonight_free_ctx(ctx2);
			cryptonight_free_ctx(ctx3);
			cryptonight_free_ctx(ctx4);
			return false;
		}
	}

	unsigned char out[32*8];
	bool bResult = true;

	if(jconf::inst()->HaveHardwareAes())
	{
		//cryptonight_hash_ctx("This is a test", 14, out, ctx0);
		//bResult = memcmp(out, "\xa0\x84\xf0\x1d\x14\x37\xa0\x9c\x69\x85\x40\x1b\x60\xd4\x35\x54\xae\x10\x58\x02\xc5\xf5\xd8\xa9\xb3\x25\x36\x49\xc0\xbe\x66\x05", 32) == 0;

		//cryptonight_hash_ctx_np("This is a test", 14, out, ctx0);
		//bResult &= memcmp(out, "\xa0\x84\xf0\x1d\x14\x37\xa0\x9c\x69\x85\x40\x1b\x60\xd4\x35\x54\xae\x10\x58\x02\xc5\xf5\xd8\xa9\xb3\x25\x36\x49\xc0\xbe\x66\x05", 32) == 0;

		uint8_t results[32*8];
		cryptonight_hash_ctx_np("nada", 4, results, ctx0);
		for(int z=1; z<7;z++)
			cryptonight_hash_ctx_np("nado", 4, results + 32*z, ctx0);

		cryptonight_ctx* ctx[8] = {ctx0, ctx1, ctx2, ctx3, ctx4, ctx4, ctx4, ctx4};
		cryptonight_double_hash_ctx("nadanadonadonadonadonadonadonado", 4, out, ctx);
		bResult = memcmp(out, results, 32*2) == 0;
	}
	else
	{
		cryptonight_hash_ctx_soft("This is a test", 14, out, ctx0);
		bResult = memcmp(out, "\xa0\x84\xf0\x1d\x14\x37\xa0\x9c\x69\x85\x40\x1b\x60\xd4\x35\x54\xae\x10\x58\x02\xc5\xf5\xd8\xa9\xb3\x25\x36\x49\xc0\xbe\x66\x05", 32) == 0;
	}

	cryptonight_free_ctx(ctx0);
	cryptonight_free_ctx(ctx1);
	cryptonight_free_ctx(ctx2);
	cryptonight_free_ctx(ctx3);
	cryptonight_free_ctx(ctx4);

	if(!bResult)
		printer::inst()->print_msg(L0,
		    "Cryptonight hash self-test failed. This might be caused by bad compiler optimizations.");

	return bResult;
}

std::vector<minethd*>* minethd::thread_starter(miner_work& pWork)
{
	iGlobalJobNo = 0;
	iConsumeCnt = 0;
	std::vector<minethd*>* pvThreads = new std::vector<minethd*>;

	//Launch the requested number of single and double threads, to distribute
	//load evenly we need to alternate single and double threads
	size_t i, n = jconf::inst()->GetThreadCount();
	pvThreads->reserve(n);

	jconf::thd_cfg cfg;
	for (i = 0; i < n; i++)
	{
		jconf::inst()->GetThreadConfig(i, cfg);

		minethd* thd = new minethd(pWork, i, cfg.bDoubleMode, cfg.bNoPrefetch);

		if(cfg.iCpuAff >= 0)
		{
#if defined(__APPLE__)
			printer::inst()->print_msg(L1, "WARNING on MacOS thread affinity is only advisory.");
#endif
			thd_setaffinity(thd->oWorkThd.native_handle(), cfg.iCpuAff);
		}

		pvThreads->push_back(thd);

		if(cfg.iCpuAff >= 0)
			printer::inst()->print_msg(L1, "Starting %s thread, affinity: %d.", cfg.bDoubleMode ? "double" : "single", (int)cfg.iCpuAff);
		else
			printer::inst()->print_msg(L1, "Starting %s thread, no affinity.", cfg.bDoubleMode ? "double" : "single");
	}

	iThreadCount = n;
	return pvThreads;
}

void minethd::switch_work(miner_work& pWork)
{
	// iConsumeCnt is a basic lock-like polling mechanism just in case we happen to push work
	// faster than threads can consume them. This should never happen in real life.
	// Pool cant physically send jobs faster than every 250ms or so due to net latency.

	while (iConsumeCnt.load(std::memory_order_seq_cst) < iThreadCount)
		std::this_thread::sleep_for(std::chrono::milliseconds(100));

	oGlobalWork = pWork;
	iConsumeCnt.store(0, std::memory_order_seq_cst);
	iGlobalJobNo++;
}

void minethd::consume_work()
{
	memcpy(&oWork, &oGlobalWork, sizeof(miner_work));
	iJobNo++;
	iConsumeCnt++;
}

void minethd::work_main()
{
	cryptonight_ctx* ctx;
	uint64_t iCount = 0;
	uint64_t* piHashVal;
	uint32_t* piNonce;
	job_result result;

	ctx = minethd_alloc_ctx();

	piHashVal = (uint64_t*)(result.bResult + 24);
	piNonce = (uint32_t*)(oWork.bWorkBlob + 39);
	iConsumeCnt++;

	bool bHaveAes = jconf::inst()->HaveHardwareAes();
	while (bQuit == 0)
	{
		if (oWork.bStall)
		{
			/*  We are stalled here because the executor didn't find a job for us yet,
			    either because of network latency, or a socket problem. Since we are
			    raison d'etre of this software it us sensible to just wait until we have something*/

			while (iGlobalJobNo.load(std::memory_order_relaxed) == iJobNo)
				std::this_thread::sleep_for(std::chrono::milliseconds(100));

			consume_work();
			continue;
		}

		if(oWork.bNiceHash)
			result.iNonce = calc_nicehash_nonce(*piNonce, oWork.iResumeCnt);
		else
			result.iNonce = calc_start_nonce(oWork.iResumeCnt);

		assert(sizeof(job_result::sJobID) == sizeof(pool_job::sJobID));
		memcpy(result.sJobID, oWork.sJobID, sizeof(job_result::sJobID));

		while(iGlobalJobNo.load(std::memory_order_relaxed) == iJobNo)
		{
			if ((iCount & 0x7) == 0) //Store stats every 16 hashes
			{
				using namespace std::chrono;
				uint64_t iStamp = time_point_cast<milliseconds>(high_resolution_clock::now()).time_since_epoch().count();
				iHashCount.store(iCount, std::memory_order_relaxed);
				iTimestamp.store(iStamp, std::memory_order_relaxed);
			}
			iCount++;

			*piNonce = ++result.iNonce;

			if(bHaveAes)
			{
				if(bNoPrefetch)
					cryptonight_hash_ctx_np(oWork.bWorkBlob, oWork.iWorkSize, result.bResult, ctx);
				else
					cryptonight_hash_ctx(oWork.bWorkBlob, oWork.iWorkSize, result.bResult, ctx);
			}
			else
				cryptonight_hash_ctx_soft(oWork.bWorkBlob, oWork.iWorkSize, result.bResult, ctx);

			if (*piHashVal < oWork.iTarget)
				executor::inst()->push_event(ex_event(result, oWork.iPoolId));

			std::this_thread::yield();
		}

		consume_work();
	}

	cryptonight_free_ctx(ctx);
}

void minethd::double_work_main()
{
	// remember to also change in crypto/cryptonight_aesni.h
	static const int hashes = 2;
	cryptonight_ctx* ctx[hashes];
	uint64_t iCount = 0;
	uint64_t *piHashVal[hashes];
	uint32_t *piNonce[hashes];
	uint8_t bDoubleHashOut[32*hashes];
	uint8_t	bDoubleWorkBlob[sizeof(miner_work::bWorkBlob) * 5];
	uint32_t iNonce;
	job_result res;

	for(int i=0; i<hashes; i++){
		ctx[i] = minethd_alloc_ctx();
		piHashVal[i] = (uint64_t*)(bDoubleHashOut + (32*i) + 24);
		piNonce[i] = nullptr;
	}
	piNonce[0] = (uint32_t*)(bDoubleWorkBlob + 39);

	iConsumeCnt++;

	while (bQuit == 0)
	{
		if (oWork.bStall)
		{
			/*	We are stalled here because the executor didn't find a job for us yet,
			either because of network latency, or a socket problem. Since we are
			raison d'etre of this software it us sensible to just wait until we have something*/

			while (iGlobalJobNo.load(std::memory_order_relaxed) == iJobNo)
				std::this_thread::sleep_for(std::chrono::milliseconds(100));

			consume_work();
			for(int i=0; i<hashes; i++){
				memcpy(bDoubleWorkBlob + i*oWork.iWorkSize, oWork.bWorkBlob, oWork.iWorkSize);
				piNonce[i] = (uint32_t*)(bDoubleWorkBlob + i*oWork.iWorkSize + 39);
			}
			continue;
		}

		if(oWork.bNiceHash)
			iNonce = calc_nicehash_nonce(*piNonce[0], oWork.iResumeCnt);
		else
			iNonce = calc_start_nonce(oWork.iResumeCnt);

		assert(sizeof(job_result::sJobID) == sizeof(pool_job::sJobID));

		while (iGlobalJobNo.load(std::memory_order_relaxed) == iJobNo)
		{
			if ((iCount & 0x7) == 0) //Store stats every 16 hashes
			{
				using namespace std::chrono;
				uint64_t iStamp = time_point_cast<milliseconds>(high_resolution_clock::now()).time_since_epoch().count();
				iHashCount.store(iCount, std::memory_order_relaxed);
				iTimestamp.store(iStamp, std::memory_order_relaxed);
			}

			iCount += hashes;

			for(int i=0; i<hashes; i++)
				*piNonce[i] = ++iNonce;
			cryptonight_double_hash_ctx(bDoubleWorkBlob, oWork.iWorkSize, bDoubleHashOut, ctx);

			for(int i=0;i<hashes;i++){
				if (*piHashVal[i] < oWork.iTarget)
					executor::inst()->push_event(ex_event(job_result(oWork.sJobID, iNonce-(hashes-i-1), bDoubleHashOut + 32*i), oWork.iPoolId));
			}

			std::this_thread::yield();
		}

		consume_work();
		for(int i=0; i<hashes; i++){
			memcpy(bDoubleWorkBlob + i*oWork.iWorkSize, oWork.bWorkBlob, oWork.iWorkSize);
			if(i>0) {piNonce[i] = (uint32_t*)(bDoubleWorkBlob + i*oWork.iWorkSize + 39);}
		}
	}

	for(int i=0; i<hashes; i++)
		cryptonight_free_ctx(ctx[i]);
}
