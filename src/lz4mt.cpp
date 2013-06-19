#include <array>
#include <atomic>
#include <cassert>
#include <future>
#include <mutex>
#include <vector>
#include <queue>
#include <string.h>
#include "lz4mt.h"
#include "lz4mt_xxh32.h"
#include "lz4mt_mempool.h"
#include "lz4mt_compat.h"
#include "lz4mt_threadpool.h"

#if 1
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include "test_clock.h"
#endif

//#define USE_THREADPOOL

namespace {

const uint32_t LZ4S_MAGICNUMBER = 0x184D2204;
const uint32_t LZ4S_MAGICNUMBER_SKIPPABLE_MIN = 0x184D2A50;
const uint32_t LZ4S_MAGICNUMBER_SKIPPABLE_MAX = 0x184D2A5F;
const uint32_t LZ4S_BLOCKSIZEID_DEFAULT = 7;
const uint32_t LZ4S_CHECKSUM_SEED = 0;
const uint32_t LZ4S_EOS = 0;
const uint32_t LZ4S_MAX_HEADER_SIZE = 4 + 2 + 8 + 4 + 1;

typedef std::unique_ptr<Lz4Mt::MemPool::Buffer> BufferPtr;

int getBlockSize(int bdBlockMaximumSize) {
	assert(bdBlockMaximumSize >= 4 && bdBlockMaximumSize <= 7);
	return (1 << (8 + (2 * bdBlockMaximumSize)));
}

uint32_t getCheckBits_FromXXH(uint32_t xxh) {
	return (xxh >> 8) & 0xff;
}

bool isSkippableMagicNumber(uint32_t magic) {
	return magic >= LZ4S_MAGICNUMBER_SKIPPABLE_MIN
		&& magic <= LZ4S_MAGICNUMBER_SKIPPABLE_MAX;
}

char flgToChar(const Lz4MtFlg& flg) {
	return static_cast<char>(
		  ((flg.presetDictionary  & 1) << 0)
		| ((flg.reserved1         & 1) << 1)
		| ((flg.streamChecksum    & 1) << 2)
		| ((flg.streamSize        & 1) << 3)
		| ((flg.blockChecksum     & 1) << 4)
		| ((flg.blockIndependance & 1) << 5)
		| ((flg.versionNumber     & 3) << 6)
	);
}

Lz4MtFlg charToFlg(char c) {
	Lz4MtFlg flg = { 0 };
	flg.presetDictionary	= (c >> 0) & 1;
	flg.reserved1			= (c >> 1) & 1;
	flg.streamChecksum		= (c >> 2) & 1;
	flg.streamSize			= (c >> 3) & 1;
	flg.blockChecksum		= (c >> 4) & 1;
	flg.blockIndependance	= (c >> 5) & 1;
	flg.versionNumber		= (c >> 6) & 3;
	return flg;
}

char bdToChar(const Lz4MtBd& bd) {
	return static_cast<char>(
		  ((bd.reserved3        & 15) << 0)
		| ((bd.blockMaximumSize &  7) << 4)
		| ((bd.reserved2        &  1) << 7)
	);
}

Lz4MtBd charToBc(char c) {
	Lz4MtBd bd = { 0 };
	bd.reserved3		= (c >> 0) & 15;
	bd.blockMaximumSize	= (c >> 4) &  7;
	bd.reserved2		= (c >> 7) &  1;
	return bd;
}

size_t storeU32(void* p, uint32_t v) {
	auto* q = reinterpret_cast<char*>(p);
	q[0] = static_cast<char>(v >> (8*0));
	q[1] = static_cast<char>(v >> (8*1));
	q[2] = static_cast<char>(v >> (8*2));
	q[3] = static_cast<char>(v >> (8*3));
	return sizeof(v);
}

size_t storeU64(void* p, uint64_t v) {
	auto* q = reinterpret_cast<char*>(p);
	storeU32(q+0, static_cast<uint32_t>(v >> (8*0)));
	storeU32(q+4, static_cast<uint32_t>(v >> (8*4)));
	return sizeof(v);
}

uint32_t loadU32(const void* p) {
	auto* q = reinterpret_cast<const uint8_t*>(p);
	return (static_cast<uint32_t>(q[0]) << (8*0))
		 | (static_cast<uint32_t>(q[1]) << (8*1))
		 | (static_cast<uint32_t>(q[2]) << (8*2))
		 | (static_cast<uint32_t>(q[3]) << (8*3));
}

uint64_t loadU64(const void* p) {
	auto* q = reinterpret_cast<const uint8_t*>(p);
	return (static_cast<uint64_t>(loadU32(q+0)) << (8*0))
		 | (static_cast<uint64_t>(loadU32(q+4)) << (8*4));
}

Lz4MtResult
validateStreamDescriptor(const Lz4MtStreamDescriptor* sd) {
	if(1 != sd->flg.versionNumber) {
		return LZ4MT_RESULT_INVALID_VERSION;
	}
	if(0 != sd->flg.presetDictionary) {
		///	@TODO: Implement Preset Dictionary.
		return LZ4MT_RESULT_PRESET_DICTIONARY_IS_NOT_SUPPORTED_YET;
	}
	if(0 != sd->flg.reserved1) {
		return LZ4MT_RESULT_INVALID_HEADER;
	}
	if(0 == sd->flg.blockIndependance) {
		///	@TODO: Implement Block Dependency. lz4: r96 - https://code.google.com/p/lz4/source/detail?r=96
		return LZ4MT_RESULT_BLOCK_DEPENDENCE_IS_NOT_SUPPORTED_YET;
	}
	if(sd->bd.blockMaximumSize < 4 || sd->bd.blockMaximumSize > 7) {
		return LZ4MT_RESULT_INVALID_BLOCK_MAXIMUM_SIZE;
	}
	if(0 != sd->bd.reserved3) {
		return LZ4MT_RESULT_INVALID_HEADER;
	}
	if(0 != sd->bd.reserved2) {
		return LZ4MT_RESULT_INVALID_HEADER;
	}
	return LZ4MT_RESULT_OK;
}

class Context {
public:
	Context(Lz4MtContext* ctx)
		: ctx(ctx)
		, mutResult()
		, mutFs()
	{}

	bool error() const {
		Lock lock(mutResult);
		return LZ4MT_RESULT_OK != ctx->result;
	}

	Lz4MtResult setResult(Lz4MtResult result) {
		Lock lock(mutResult);
		auto& r = ctx->result;
		if(LZ4MT_RESULT_OK == r || LZ4MT_RESULT_ERROR == r) {
			r = result;
		}
		return r;
	}

	Lz4MtResult result() {
		Lock lock(mutResult);
		return ctx->result;
	}

	uint32_t readU32() {
		if(error()) {
			return 0;
		}

		char d[sizeof(uint32_t)];
		if(sizeof(d) != read(d, sizeof(d))) {
			setResult(LZ4MT_RESULT_ERROR);
			return 0;
		}
		return loadU32(d);
	}

	bool writeU32(uint32_t v) {
		if(error()) {
			return false;
		}

		char d[sizeof(v)];
		storeU32(d, v);
		if(sizeof(d) != write(d, sizeof(d))) {
			setResult(LZ4MT_RESULT_ERROR);
			return false;
		}
		return true;
	}

	bool writeBin(const void* ptr, int size) {
		if(error()) {
			return false;
		}
		if(size != write(ptr, size)) {
			setResult(LZ4MT_RESULT_ERROR);
			return false;
		}
		return true;
	}

	Lz4MtMode mode() const {
		return ctx->mode;
	}

	int read(void* dst, int dstSize) {
		Lock lock(mutFs);
		return ctx->read(ctx, dst, dstSize);
	}

	int readSeek(int offset) {
		Lock lock(mutFs);
		return ctx->readSeek(ctx, offset);
	}

	int readEof() {
		Lock lock(mutFs);
		return ctx->readEof(ctx);
	}

	int readSkippable(uint32_t magicNumber, size_t size) {
		Lock lock(mutFs);
		return ctx->readSkippable(ctx, magicNumber, size);
	}

	int write(const void* src, int srcSize) {
		Lock lock(mutFs);
		return ctx->write(ctx, src, srcSize);
	}

	int compress(const char* src, char* dst, int isize, int maxOutputSize) {
		return ctx->compress(src, dst, isize, maxOutputSize);
	}

	int decompress(const char* src, char* dst, int isize, int maxOutputSize) {
		return ctx->decompress(src, dst, isize, maxOutputSize);
	}

private:
	typedef std::unique_lock<std::mutex> Lock;
	Lz4MtContext* ctx;
	mutable std::mutex mutResult;
	mutable std::mutex mutFs;
};


#if defined(USE_THREADPOOL)
class TaskWait {
public:
	TaskWait()
		: stop(false)
		, mut()
		, cond()
		, lastIndex(0)
		, bits()
	{
		Lock lock(mut);

		bits.resize(1024 * 1024);
		memset(bits.data(), 0, bits.size() * sizeof(bits[0]));
	}

	~TaskWait() {
		{
			Lock lock(mut);
			stop = true;
		}
		cond.notify_all();
	}

	void wait(int i_) {
		if(i_ <= 0) {
			return;
		}
		const auto i1 = i_ - 1;
		const auto i132 = i1 / 32;
		const auto m = 1 << (i1 & 31);
		for(;;) {
			volatile auto b = bits[i132] & m;
			if(b != 0) {
				return;
			}
			if(stop) {
				return;
			}

			Lock lock(mut);
			cond.wait(lock);
		}
	}

	void done(int i) {
		Lock lock(mut);
		bits[i/32] |= 1 << (i & 31);
		if(lastIndex < i) {
			lastIndex = i;
		}
		cond.notify_all();
	}

private:
	typedef std::unique_lock<std::mutex> Lock;
	std::atomic<bool> stop;
	mutable std::mutex mut;
	std::condition_variable cond;
	int lastIndex;

	std::vector<uint32_t> bits;
};
#endif


} // anonymous namespace


extern "C" Lz4MtContext
lz4mtInitContext()
{
	Lz4MtContext e = { LZ4MT_RESULT_OK, 0 };

	e.result		= LZ4MT_RESULT_OK;
	e.readCtx		= nullptr;
	e.read			= nullptr;
	e.readEof		= nullptr;
	e.readSkippable	= nullptr;
	e.readSeek		= nullptr;
	e.writeCtx		= nullptr;
	e.write			= nullptr;
	e.compress		= nullptr;
	e.compressBound	= nullptr;
	e.decompress	= nullptr;
	e.mode			= LZ4MT_MODE_PARALLEL;

	return e;
}


extern "C" Lz4MtStreamDescriptor
lz4mtInitStreamDescriptor()
{
	Lz4MtStreamDescriptor e = { { 0 } };

	e.flg.presetDictionary	= 0;
	e.flg.streamChecksum	= 1;
	e.flg.reserved1			= 0;
	e.flg.streamSize		= 0;
	e.flg.blockChecksum		= 0;
	e.flg.blockIndependance	= 1;
	e.flg.versionNumber		= 1;

	e.bd.reserved3			= 0;
	e.bd.blockMaximumSize	= LZ4S_BLOCKSIZEID_DEFAULT;
	e.bd.reserved2			= 0;

	e.streamSize			= 0;
	e.dictId				= 0;

	return e;
}


extern "C" const char*
lz4mtResultToString(Lz4MtResult result)
{
	const char* s = "???";
	switch(result) {
	case LZ4MT_RESULT_OK:
		s = "OK";
		break;
	case LZ4MT_RESULT_ERROR:
		s = "ERROR";
		break;
	case LZ4MT_RESULT_INVALID_MAGIC_NUMBER:
		s = "INVALID_MAGIC_NUMBER";
		break;
	case LZ4MT_RESULT_INVALID_HEADER:
		s = "INVALID_HEADER";
		break;
	case LZ4MT_RESULT_PRESET_DICTIONARY_IS_NOT_SUPPORTED_YET:
		s = "PRESET_DICTIONARY_IS_NOT_SUPPORTED_YET";
		break;
	case LZ4MT_RESULT_BLOCK_DEPENDENCE_IS_NOT_SUPPORTED_YET:
		s = "BLOCK_DEPENDENCE_IS_NOT_SUPPORTED_YET";
		break;
	case LZ4MT_RESULT_INVALID_VERSION:
		s = "INVALID_VERSION";
		break;
	case LZ4MT_RESULT_INVALID_HEADER_CHECKSUM:
		s = "INVALID_HEADER_CHECKSUM";
		break;
	case LZ4MT_RESULT_INVALID_BLOCK_MAXIMUM_SIZE:
		s = "INVALID_BLOCK_MAXIMUM_SIZE";
		break;
	case LZ4MT_RESULT_CANNOT_WRITE_HEADER:
		s = "CANNOT_WRITE_HEADER";
		break;
	case LZ4MT_RESULT_CANNOT_WRITE_EOS:
		s = "CANNOT_WRITE_EOS";
		break;
	case LZ4MT_RESULT_CANNOT_WRITE_STREAM_CHECKSUM:
		s = "CANNOT_WRITE_STREAM_CHECKSUM";
		break;
	case LZ4MT_RESULT_CANNOT_READ_BLOCK_SIZE:
		s = "CANNOT_READ_BLOCK_SIZE";
		break;
	case LZ4MT_RESULT_CANNOT_READ_BLOCK_DATA:
		s = "CANNOT_READ_BLOCK_DATA";
		break;
	case LZ4MT_RESULT_CANNOT_READ_BLOCK_CHECKSUM:
		s = "CANNOT_READ_BLOCK_CHECKSUM";
		break;
	case LZ4MT_RESULT_CANNOT_READ_STREAM_CHECKSUM:
		s = "CANNOT_READ_STREAM_CHECKSUM";
		break;
	case LZ4MT_RESULT_STREAM_CHECKSUM_MISMATCH:
		s = "STREAM_CHECKSUM_MISMATCH";
		break;
	case LZ4MT_RESULT_DECOMPRESS_FAIL:
		s = "DECOMPRESS_FAIL";
		break;
	default:
		s = "Unknown code";
		break;
	}
	return s;
}


extern "C" Lz4MtResult
lz4mtCompress(Lz4MtContext* lz4MtContext, const Lz4MtStreamDescriptor* sd)
{
	assert(lz4MtContext);
	assert(sd);

	Context ctx_(lz4MtContext);
	Context* ctx = &ctx_;

	{
		char d[LZ4S_MAX_HEADER_SIZE] = { 0 };
		auto p = &d[0];

		const auto r = validateStreamDescriptor(sd);
		if(LZ4MT_RESULT_OK != r) {
			return ctx->setResult(r);
		}
		p += storeU32(p, LZ4S_MAGICNUMBER);

		const auto* sumBegin = p;
		*p++ = flgToChar(sd->flg);
		*p++ = bdToChar(sd->bd);
		if(sd->flg.streamSize) {
			assert(sd->streamSize);
			p += storeU64(p, sd->streamSize);
		}
		if(sd->flg.presetDictionary) {
			p += storeU32(p, sd->dictId);
		}

		const auto sumSize = static_cast<int>(p - sumBegin);
		const auto h = Lz4Mt::Xxh32(sumBegin, sumSize, LZ4S_CHECKSUM_SEED).digest();
		*p++ = static_cast<char>(getCheckBits_FromXXH(h));
		assert(p <= std::end(d));

		const auto writeSize = static_cast<int>(p - d);
		if(writeSize != ctx->write(d, writeSize)) {
			return ctx->setResult(LZ4MT_RESULT_CANNOT_WRITE_HEADER);
		}
	}

	const auto nBlockMaximumSize = getBlockSize(sd->bd.blockMaximumSize);
	const auto nBlockSize        = 4;
	const auto nBlockCheckSum    = sd->flg.blockChecksum ? 4 : 0;
	const auto cIncompressible   = 1 << (nBlockSize * 8 - 1);
	const bool streamChecksum    = 0 != sd->flg.streamChecksum;
	const bool singleThread      = 0 != (ctx->mode() & LZ4MT_MODE_SEQUENTIAL);
	const auto nConcurrency      = Lz4Mt::getHardwareConcurrency();
	const auto nPool             = singleThread ? 1 : nConcurrency + 1;

	Lz4Mt::MemPool srcBufferPool(nBlockMaximumSize, nPool);
	Lz4Mt::MemPool dstBufferPool(nBlockMaximumSize, nPool);
#if defined(USE_THREADPOOL)
//	Lz4Mt::ThreadPool threadPool;
	Lz4Mt::ThreadPool threadPool(nPool);
	TaskWait taskWait;
#else
	std::vector<std::future<void>> futures;
#endif

#if 0 && defined(USE_THREADPOOL)
#else
	const auto launch = singleThread ? Lz4Mt::launch::deferred : std::launch::async;
#endif
	Lz4Mt::Xxh32 xxhStream(LZ4S_CHECKSUM_SEED);

#if 1
//	double tt = 0.0;
#endif

	const auto f =
#if defined(USE_THREADPOOL)
		[&taskWait, &dstBufferPool, &xxhStream, launch, &tt
		 , ctx, nBlockCheckSum, streamChecksum, cIncompressible
		 ]
#else
		[&futures, &dstBufferPool, &xxhStream //, &tt
		 , ctx, nBlockCheckSum, streamChecksum, launch, cIncompressible
		 ]
#endif
	(int i, Lz4Mt::MemPool::Buffer* srcRawPtr, int srcSize)
	{
		BufferPtr src(srcRawPtr);
		if(ctx->error()) {
			return;
		}

		const auto* srcPtr = src->data();
		BufferPtr dst(dstBufferPool.alloc());
		auto* cmpPtr = dst->data();
		const auto cmpSize = ctx->compress(srcPtr, cmpPtr, srcSize, srcSize);
		const bool incompressible = (cmpSize <= 0);
		const auto* cPtr  = incompressible ? srcPtr  : cmpPtr;
		const auto  cSize = incompressible ? srcSize : cmpSize;

#if 0 && defined(USE_THREADPOOL)
		uint32_t blockHash = 0;
		if(nBlockCheckSum) {
			blockHash = Lz4Mt::Xxh32(cPtr, cSize, LZ4S_CHECKSUM_SEED).digest();
		}
#else
		std::future<uint32_t> futureBlockHash;
		if(nBlockCheckSum) {
			futureBlockHash = std::async(launch, [=] {
				return Lz4Mt::Xxh32(cPtr, cSize, LZ4S_CHECKSUM_SEED).digest();
			});
		}
#endif
		if(incompressible) {
			dst.reset();
		}

#if defined(USE_THREADPOOL)
		if(i > 0) {
//			const auto t0 = Clock::now();
			taskWait.wait(i);
//			const auto t1 = Clock::now();
//			tt += std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0).count();
		}
#else
		if(i > 0) {
//			const auto t0 = Clock::now();
			futures[i-1].wait();
//			const auto t1 = Clock::now();
//			tt += std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0).count();
		}
#endif

#if 0 && defined(USE_THREADPOOL)
		if(streamChecksum) {
			xxhStream.update(srcPtr, srcSize);
		}
#else
		std::future<void> futureStreamHash;
		if(streamChecksum) {
			futureStreamHash = std::async(launch, [=, &xxhStream] {
				xxhStream.update(srcPtr, srcSize);
			});
		}
#endif

		if(incompressible) {
			ctx->writeU32(cSize | cIncompressible);
			ctx->writeBin(srcPtr, srcSize);
		} else {
			ctx->writeU32(cSize);
			ctx->writeBin(cmpPtr, cmpSize);
		}

#if 0 && defined(USE_THREADPOOL)
		if(nBlockCheckSum) {
			ctx->writeU32(blockHash);
		}
#else
		if(futureBlockHash.valid()) {
			ctx->writeU32(futureBlockHash.get());
		}
		if(futureStreamHash.valid()) {
			futureStreamHash.wait();
		}
#endif

#if defined(USE_THREADPOOL)
		taskWait.done(i);
#else
#endif
	};

	for(int i = 0;; ++i) {
		auto* src = srcBufferPool.alloc();
		auto* srcPtr = src->data();
		const auto srcSize = src->size();
		const auto readSize = ctx->read(srcPtr, static_cast<int>(srcSize));

		if(0 == readSize) {
			break;
		}

		if(singleThread) {
			f(0, src, readSize);
		} else {
#if defined(USE_THREADPOOL)
			threadPool.enqueue(
				[&f, i, src, readSize](int threadIndex) {
					(void)(threadIndex);
					f(i, src, readSize);
				}
			);
#else
			futures.emplace_back(std::async(launch, f, i, src, readSize));
#endif
		}
	}

#if defined(USE_THREADPOOL)
	threadPool.joinAll();
#else
	for(auto& e : futures) {
		e.wait();
	}
#endif

	if(!ctx->writeU32(LZ4S_EOS)) {
		return LZ4MT_RESULT_CANNOT_WRITE_EOS;
	}

	if(streamChecksum) {
		const auto digest = xxhStream.digest();
		if(!ctx->writeU32(digest)) {
			return LZ4MT_RESULT_CANNOT_WRITE_STREAM_CHECKSUM;
		}
//		printf("xxhStream.digest = %08x    tt=%10.6f  ", digest, tt);
	}

	return LZ4MT_RESULT_OK;
}


extern "C" Lz4MtResult
lz4mtDecompress(Lz4MtContext* lz4MtContext, Lz4MtStreamDescriptor* sd)
{
	assert(lz4MtContext);
	assert(sd);

	Context ctx_(lz4MtContext);
	Context* ctx = &ctx_;

	std::atomic<bool> quit(false);

	ctx->setResult(LZ4MT_RESULT_OK);
	while(!quit && !ctx->error() && !ctx->readEof()) {
		const auto magic = ctx->readU32();
		if(ctx->error()) {
			if(ctx->readEof()) {
				ctx->setResult(LZ4MT_RESULT_OK);
			} else {
				ctx->setResult(LZ4MT_RESULT_INVALID_HEADER);
			}
			break;
		}

		if(isSkippableMagicNumber(magic)) {
			const auto size = ctx->readU32();
			if(ctx->error()) {
				ctx->setResult(LZ4MT_RESULT_INVALID_HEADER);
				break;
			}
			const auto s = ctx->readSkippable(magic, size);
			if(s < 0 || ctx->error()) {
				ctx->setResult(LZ4MT_RESULT_INVALID_HEADER);
				break;
			}
			continue;
		}

		if(LZ4S_MAGICNUMBER != magic) {
			ctx->readSeek(-4);
			ctx->setResult(LZ4MT_RESULT_INVALID_MAGIC_NUMBER);
			break;
		}

		char d[LZ4S_MAX_HEADER_SIZE] = { 0 };
		auto* p = d;
		const auto* sumBegin = p;

		if(2 != ctx->read(p, 2)) {
			ctx->setResult(LZ4MT_RESULT_INVALID_HEADER);
			break;
		}
		sd->flg = charToFlg(*p++);
		sd->bd  = charToBc(*p++);
		const auto r = validateStreamDescriptor(sd);
		if(LZ4MT_RESULT_OK != r) {
			ctx->setResult(r);
			break;
		}

		const int nExInfo =
			  (sd->flg.streamSize       ? sizeof(uint64_t) : 0)
			+ (sd->flg.presetDictionary ? sizeof(uint32_t) : 0)
			+ 1
		;
		if(nExInfo != ctx->read(p, nExInfo)) {
			ctx->setResult(LZ4MT_RESULT_INVALID_HEADER);
			break;
		}

		if(sd->flg.streamSize) {
			sd->streamSize = loadU64(p);
			p += sizeof(uint64_t);
		}

		if(sd->flg.presetDictionary) {
			sd->dictId = loadU32(p);
			p += sizeof(uint32_t);
		}

		const auto sumSize   = static_cast<int>(p - sumBegin);
		const auto calHash32 = Lz4Mt::Xxh32(sumBegin, sumSize, LZ4S_CHECKSUM_SEED).digest();
		const auto calHash   = static_cast<char>(getCheckBits_FromXXH(calHash32));
		const auto srcHash   = *p++;

		assert(p <= std::end(d));

		if(srcHash != calHash) {
			ctx->setResult(LZ4MT_RESULT_INVALID_HEADER_CHECKSUM);
			break;
		}

		const auto nBlockMaximumSize = getBlockSize(sd->bd.blockMaximumSize);
		const auto nBlockCheckSum    = sd->flg.blockChecksum ? 4 : 0;
		const bool streamChecksum    = 0 != sd->flg.streamChecksum;
		const bool singleThread      = 0 != (ctx->mode() & LZ4MT_MODE_SEQUENTIAL);
		const auto nConcurrency      = Lz4Mt::getHardwareConcurrency();
		const auto nPool             = singleThread ? 1 : nConcurrency + 1;

		Lz4Mt::MemPool srcBufferPool(nBlockMaximumSize, nPool);
		Lz4Mt::MemPool dstBufferPool(nBlockMaximumSize, nPool);
#if defined(USE_THREADPOOL)
		Lz4Mt::ThreadPool threadPool;
		TaskWait taskWait;
#else
		const auto launch = singleThread ? Lz4Mt::launch::deferred : std::launch::async;
		std::vector<std::future<Lz4MtResult>> futures;
#endif
		Lz4Mt::Xxh32 xxhStream(LZ4S_CHECKSUM_SEED);

		const auto f = [
#if defined(USE_THREADPOOL)
			&threadPool, &taskWait, &dstBufferPool, &xxhStream, &quit
			, ctx, nBlockCheckSum, streamChecksum
			] (int i, Lz4Mt::MemPool::Buffer* srcRaw, bool incompressible, uint32_t blockChecksum)
		  -> void
#else
			&futures, &dstBufferPool, &xxhStream, &quit
			, ctx, nBlockCheckSum, streamChecksum, launch
			] (int i, Lz4Mt::MemPool::Buffer* srcRaw, bool incompressible, uint32_t blockChecksum)
		  -> Lz4MtResult
#endif
		{
			BufferPtr src(srcRaw);
			if(ctx->error() || quit) {
#if defined(USE_THREADPOOL)
				return;
#else
				return LZ4MT_RESULT_OK;
#endif
			}

			const auto* srcPtr = src->data();
			const auto srcSize = static_cast<int>(src->size());

#if defined(USE_THREADPOOL)
			uint32_t blockHash = 0;
			if(nBlockCheckSum) {
				blockHash = Lz4Mt::Xxh32(srcPtr, srcSize, LZ4S_CHECKSUM_SEED).digest();
			}
#else
			std::future<uint32_t> futureBlockHash;
			if(nBlockCheckSum) {
				futureBlockHash = std::async(launch, [=] {
					return Lz4Mt::Xxh32(srcPtr, srcSize, LZ4S_CHECKSUM_SEED).digest();
				});
			}
#endif

			if(incompressible) {
#if defined(USE_THREADPOOL)
				if(i > 0) {
					taskWait.wait(i);
				}
				if(streamChecksum) {
					xxhStream.update(srcPtr, srcSize);
				}
#else
				if(i > 0) {
					futures[i-1].wait();
				}
				std::future<void> futureStreamHash;
				if(streamChecksum) {
					futureStreamHash = std::async(
						  launch
						, [&xxhStream, srcPtr, srcSize] {
							xxhStream.update(srcPtr, srcSize);
						}
					);
				}
#endif
				ctx->writeBin(srcPtr, srcSize);
#if defined(USE_THREADPOOL)
#else
				if(futureStreamHash.valid()) {
					futureStreamHash.wait();
				}
#endif
			} else {
				BufferPtr dst(dstBufferPool.alloc());

				auto* dstPtr = dst->data();
				const auto dstSize = dst->size();
				const auto decSize = ctx->decompress(
					srcPtr, dstPtr, srcSize, static_cast<int>(dstSize));
				if(decSize < 0) {
					quit = true;
#if defined(USE_THREADPOOL)
					ctx->setResult(LZ4MT_RESULT_DECOMPRESS_FAIL);
					return;
#else
					return LZ4MT_RESULT_DECOMPRESS_FAIL;
#endif
				}

#if defined(USE_THREADPOOL)
				if(i > 0) {
					taskWait.wait(i);
				}
				if(streamChecksum) {
					xxhStream.update(dstPtr, decSize);
				}
#else
				if(i > 0) {
					futures[i-1].wait();
				}

				std::future<void> futureStreamHash;
				if(streamChecksum) {
					futureStreamHash = std::async(
						  launch
						, [&xxhStream, dstPtr, decSize] {
							xxhStream.update(dstPtr, decSize);
						}
					);
				}
#endif
				ctx->writeBin(dstPtr, decSize);
#if defined(USE_THREADPOOL)
#else
				if(futureStreamHash.valid()) {
					futureStreamHash.wait();
				}
#endif
			}

#if defined(USE_THREADPOOL)
			if(nBlockCheckSum) {
				if(blockHash != blockChecksum) {
					quit = true;
					ctx->setResult(LZ4MT_RESULT_BLOCK_CHECKSUM_MISMATCH);
					return;
				}
			}
#else
			if(futureBlockHash.valid()) {
				auto bh = futureBlockHash.get();
				if(bh != blockChecksum) {
					quit = true;
					return LZ4MT_RESULT_BLOCK_CHECKSUM_MISMATCH;
				}
			}
#endif

#if defined(USE_THREADPOOL)
			taskWait.done(i);
			return;
#else
			return LZ4MT_RESULT_OK;
#endif
		};

#if defined(USE_THREADPOOL)
		int lastI = 0;
#else
#endif
		for(int i = 0; !quit && !ctx->readEof(); ++i) {
			const auto srcBits = ctx->readU32();
			if(ctx->error()) {
				quit = true;
				ctx->setResult(LZ4MT_RESULT_CANNOT_READ_BLOCK_SIZE);
				break;
			}

			if(LZ4S_EOS == srcBits) {
				break;
			}

			const auto incompMask     = (1 << 31);
			const bool incompressible = 0 != (srcBits & incompMask);
			const auto srcSize        = static_cast<int>(srcBits & ~incompMask);

			auto src = srcBufferPool.alloc();
			const auto readSize = ctx->read(src->data(), srcSize);
			if(srcSize != readSize || ctx->error()) {
				quit = true;
				ctx->setResult(LZ4MT_RESULT_CANNOT_READ_BLOCK_DATA);
				break;
			}
			src->resize(readSize);

			const auto blockCheckSum = nBlockCheckSum ? ctx->readU32() : 0;
			if(ctx->error()) {
				quit = true;
				ctx->setResult(LZ4MT_RESULT_CANNOT_READ_BLOCK_CHECKSUM);
				break;
			}

			if(singleThread) {
				f(0, src, incompressible, blockCheckSum);
			} else {
#if defined(USE_THREADPOOL)
				threadPool.enqueue(
					[&f, i, src, incompressible, blockCheckSum](int threadIndex) {
						(void)(threadIndex);
						f(i, src, incompressible, blockCheckSum);
					}
				);
				lastI = i;
#else
				futures.emplace_back(std::async(
					  launch
					, f, i, src, incompressible, blockCheckSum
				));
#endif
			}
		}

#if defined(USE_THREADPOOL)
		taskWait.wait(lastI);
		threadPool.joinAll();
#else
		for(auto& e : futures) {
			const auto r = e.get();
			if(LZ4MT_RESULT_OK != r) {
				ctx->setResult(r);
			}
		}
#endif
		if(!ctx->error() && streamChecksum) {
			const auto srcStreamChecksum = ctx->readU32();
			if(ctx->error()) {
				ctx->setResult(LZ4MT_RESULT_CANNOT_READ_STREAM_CHECKSUM);
				break;
			}
			if(xxhStream.digest() != srcStreamChecksum) {
				printf("ERROR: xxhStream.digest() = %08x, srcStreamChecksum = %08x\n"
					   , xxhStream.digest(), srcStreamChecksum);
				ctx->setResult(LZ4MT_RESULT_STREAM_CHECKSUM_MISMATCH);
				break;
			}
		}
	}

	return ctx->result();
}
