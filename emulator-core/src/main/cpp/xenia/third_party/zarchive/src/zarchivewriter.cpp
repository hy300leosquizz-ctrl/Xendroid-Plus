#include "zarchive/zarchivewriter.h"
#include "zarchive/zarchivecommon.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <queue>
#include <string>
#include <string_view>

#include <zstd.h>

#include "sha_256.h"

namespace {
constexpr int kZstdCompressionLevel = 6;
}

ZArchiveWriter::ZArchiveWriter(CB_NewOutputFile cbNewOutputFile,
                               CB_WriteOutputData cbWriteOutputData, void* ctx)
    : m_cbNewOutputFile(cbNewOutputFile),
      m_cbWriteOutputData(cbWriteOutputData),
      m_cbCtx(ctx)
{
	cbNewOutputFile(-1, ctx);
	m_mainShaCtx = (struct Sha_256*)malloc(sizeof(struct Sha_256));
	sha_256_init(m_mainShaCtx, m_integritySha);

	m_currentWriteBuffer.reserve(_ZARCHIVE::COMPRESSED_BLOCK_SIZE);
	m_pendingRawBlocks.resize(kBatchBlocks * _ZARCHIVE::COMPRESSED_BLOCK_SIZE);
	const size_t maxCompressedSize = ZSTD_compressBound(_ZARCHIVE::COMPRESSED_BLOCK_SIZE);
	for (auto& buf : m_batchCompressed)
	{
		buf.resize(maxCompressedSize);
	}
	m_inlineCctx = ZSTD_createCCtx();

	const unsigned int hwCores = std::thread::hardware_concurrency();
	const unsigned int totalThreads = std::clamp(hwCores ? hwCores : 4u, 2u, 8u);
	// Spawn (totalThreads - 1) background workers; caller participates in each batch too.
	const unsigned int bgWorkers = totalThreads - 1;
	m_workers.reserve(bgWorkers);
	for (unsigned int i = 0; i < bgWorkers; ++i)
	{
		m_workers.emplace_back(&ZArchiveWriter::WorkerLoop, this);
	}
}

ZArchiveWriter::~ZArchiveWriter()
{
	ShutdownWorkers();
	if (m_inlineCctx)
	{
		ZSTD_freeCCtx(m_inlineCctx);
		m_inlineCctx = nullptr;
	}
	if (m_mainShaCtx)
	{
		free(m_mainShaCtx);
		m_mainShaCtx = nullptr;
	}
	for (PathNode* child : m_rootNode.subnodes)
	{
		DestroySubtree(child);
	}
	m_rootNode.subnodes.clear();
}

void ZArchiveWriter::DestroySubtree(PathNode* node)
{
	if (!node)
		return;
	for (PathNode* child : node->subnodes)
	{
		DestroySubtree(child);
	}
	delete node;
}

void ZArchiveWriter::ShutdownWorkers()
{
	if (m_workers.empty())
		return;
	{
		std::lock_guard<std::mutex> lock(m_batchMutex);
		m_workersQuit = true;
	}
	m_batchStartCv.notify_all();
	for (auto& t : m_workers)
	{
		if (t.joinable())
			t.join();
	}
	m_workers.clear();
}

void ZArchiveWriter::CompressBatchRange(ZSTD_CCtx_s* cctx, size_t batchCount)
{
	while (true)
	{
		const size_t idx = m_nextBlockIndex.fetch_add(1, std::memory_order_relaxed);
		if (idx >= batchCount)
			break;

		const uint8_t* src = m_pendingRawBlocks.data() + idx * _ZARCHIVE::COMPRESSED_BLOCK_SIZE;
		uint8_t* dst = m_batchCompressed[idx].data();
		const size_t dstCap = m_batchCompressed[idx].size();
		const size_t outSize = cctx
			? ZSTD_compressCCtx(cctx, dst, dstCap, src, _ZARCHIVE::COMPRESSED_BLOCK_SIZE, kZstdCompressionLevel)
			: ZSTD_compress(dst, dstCap, src, _ZARCHIVE::COMPRESSED_BLOCK_SIZE, kZstdCompressionLevel);
		m_batchSizes[idx] = ZSTD_isError(outSize) ? _ZARCHIVE::COMPRESSED_BLOCK_SIZE : outSize;

		if (m_completedBlocks.fetch_add(1, std::memory_order_acq_rel) + 1 == batchCount)
		{
			std::lock_guard<std::mutex> lock(m_batchMutex);
			m_batchDoneCv.notify_one();
		}
	}
}

void ZArchiveWriter::WorkerLoop()
{
	ZSTD_CCtx_s* workerCctx = ZSTD_createCCtx();
	uint64_t seenEpoch = 0;

	while (true)
	{
		size_t batchCount = 0;
		{
			std::unique_lock<std::mutex> lock(m_batchMutex);
			m_batchStartCv.wait(lock, [&]() {
				return m_workersQuit || m_batchEpoch != seenEpoch;
			});
			if (m_workersQuit)
				break;
			seenEpoch = m_batchEpoch;
			batchCount = m_activeBatchCount;
		}
		CompressBatchRange(workerCctx, batchCount);
	}

	if (workerCctx)
	{
		ZSTD_freeCCtx(workerCctx);
	}
}

ZArchiveWriter::PathNode* ZArchiveWriter::GetNodeByPath(ZArchiveWriter::PathNode* /*root*/, std::string_view path)
{
	PathNode* currentNode = &m_rootNode;

	std::string_view pathParser = path;
	while (true)
	{
		std::string_view nodeName;
		if (!_ZARCHIVE::GetNextPathNode(pathParser, nodeName))
			break;
		PathNode* nextSubnode = FindSubnodeByName(currentNode, nodeName);
		if (!nextSubnode || nextSubnode->isFile)
			return nullptr;
		currentNode = nextSubnode;
	}
	return currentNode;
}

ZArchiveWriter::PathNode* ZArchiveWriter::FindSubnodeByName(ZArchiveWriter::PathNode* parent, std::string_view nodeName)
{
	for (auto& it : parent->subnodes)
	{
		std::string_view itName = m_nodeNames[it->nameIndex];
		if (_ZARCHIVE::CompareNodeNameBool(itName, nodeName))
			return it;
	}
	return nullptr;
}

bool ZArchiveWriter::StartNewFile(const char* path)
{
	m_currentFileNode = nullptr;
	std::string_view pathParser = path;
	std::string_view filename;
	_ZARCHIVE::SplitFilenameFromPath(pathParser, filename);
	PathNode* dir = GetNodeByPath(&m_rootNode, pathParser);
	if (!dir)
		return false;
	if (FindSubnodeByName(dir, filename))
		return false;
	PathNode*& r = dir->subnodes.emplace_back(new PathNode(true, CreateNameEntry(filename)));
	m_currentFileNode = r;
	r->fileOffset = m_currentInputOffset;
	return true;
}

bool ZArchiveWriter::MakeDir(const char* path, bool recursive)
{
	std::string_view pathParser = path;
	while (!pathParser.empty() && (pathParser.back() == '/' || pathParser.back() == '\\'))
		pathParser.remove_suffix(1);
	if (!recursive)
	{
		std::string_view dirName;
		_ZARCHIVE::SplitFilenameFromPath(pathParser, dirName);
		PathNode* dir = GetNodeByPath(&m_rootNode, pathParser);
		if (!dir)
			return false;
		if (FindSubnodeByName(dir, dirName))
			return false;
		dir->subnodes.emplace_back(new PathNode(false, CreateNameEntry(dirName)));
	}
	else
	{
		PathNode* currentNode = &m_rootNode;
		while (true)
		{
			std::string_view nodeName;
			if (!_ZARCHIVE::GetNextPathNode(pathParser, nodeName))
				break;
			PathNode* nextSubnode = FindSubnodeByName(currentNode, nodeName);
			if (nextSubnode && nextSubnode->isFile)
				return false;
			if (!nextSubnode)
			{
				PathNode*& r = currentNode->subnodes.emplace_back(new PathNode(false, CreateNameEntry(nodeName)));
				nextSubnode = r;
			}
			currentNode = nextSubnode;
		}
	}
	return true;
}

uint32_t ZArchiveWriter::CreateNameEntry(std::string_view name)
{
	auto it = m_nodeNameLookup.find(std::string(name));
	if (it != m_nodeNameLookup.end())
		return it->second;
	uint32_t nameIndex = (uint32_t)m_nodeNames.size();
	m_nodeNames.emplace_back(name);
	m_nodeNameLookup.emplace(name, nameIndex);
	return nameIndex;
}

void ZArchiveWriter::OutputData(const void* data, size_t length)
{
	m_cbWriteOutputData(data, length, m_cbCtx);
	m_currentCompressedWriteIndex += length;
	if (m_mainShaCtx)
		sha_256_write(m_mainShaCtx, data, length);
}

uint64_t ZArchiveWriter::GetCurrentOutputOffset() const
{
	return m_currentCompressedWriteIndex;
}

void ZArchiveWriter::EmitCompressedBlock(const uint8_t* rawBlock,
                                         const uint8_t* compressedData,
                                         size_t compressedSize)
{
	const uint64_t compressedWriteOffset = GetCurrentOutputOffset();
	size_t outputSize = compressedSize;
	if (outputSize >= _ZARCHIVE::COMPRESSED_BLOCK_SIZE)
	{
		outputSize = _ZARCHIVE::COMPRESSED_BLOCK_SIZE;
		OutputData(rawBlock, _ZARCHIVE::COMPRESSED_BLOCK_SIZE);
	}
	else
	{
		OutputData(compressedData, outputSize);
	}
	if ((m_numWrittenOffsetRecords % _ZARCHIVE::ENTRIES_PER_OFFSETRECORD) == 0)
		m_compressionOffsetRecord.emplace_back().baseOffset = compressedWriteOffset;
	m_compressionOffsetRecord.back().size[m_numWrittenOffsetRecords % _ZARCHIVE::ENTRIES_PER_OFFSETRECORD] = (uint16_t)outputSize - 1;
	m_numWrittenOffsetRecords++;
}

void ZArchiveWriter::EnqueueBlock(const uint8_t* uncompressedData)
{
	std::memcpy(
		m_pendingRawBlocks.data() + m_pendingBlockCount * _ZARCHIVE::COMPRESSED_BLOCK_SIZE,
		uncompressedData,
		_ZARCHIVE::COMPRESSED_BLOCK_SIZE);
	if (++m_pendingBlockCount == kBatchBlocks)
	{
		FlushPendingBlocks();
	}
}

void ZArchiveWriter::FlushPendingBlocks()
{
	if (m_pendingBlockCount == 0)
		return;

	const size_t count = m_pendingBlockCount;
	if (count == 1 || m_workers.empty())
	{
		const uint8_t* src = m_pendingRawBlocks.data();
		uint8_t* dst = m_batchCompressed[0].data();
		const size_t dstCap = m_batchCompressed[0].size();
		const size_t outSize = m_inlineCctx
			? ZSTD_compressCCtx(m_inlineCctx, dst, dstCap, src, _ZARCHIVE::COMPRESSED_BLOCK_SIZE, kZstdCompressionLevel)
			: ZSTD_compress(dst, dstCap, src, _ZARCHIVE::COMPRESSED_BLOCK_SIZE, kZstdCompressionLevel);
		const size_t validSize = ZSTD_isError(outSize) ? _ZARCHIVE::COMPRESSED_BLOCK_SIZE : outSize;
		EmitCompressedBlock(src, dst, validSize);
		m_pendingBlockCount = 0;
		return;
	}

	{
		std::lock_guard<std::mutex> lock(m_batchMutex);
		m_activeBatchCount = count;
		m_nextBlockIndex.store(0, std::memory_order_relaxed);
		m_completedBlocks.store(0, std::memory_order_relaxed);
		++m_batchEpoch;
	}
	m_batchStartCv.notify_all();

	// Main thread participates in compressing the batch, then waits for stragglers.
	CompressBatchRange(m_inlineCctx, count);
	{
		std::unique_lock<std::mutex> lock(m_batchMutex);
		m_batchDoneCv.wait(lock, [&]() {
			return m_completedBlocks.load(std::memory_order_acquire) == count;
		});
	}

	// Retire compressed blocks strictly in sequence so archive layout & SHA-256
	// match single-threaded ZArchive bit-for-bit.
	for (size_t i = 0; i < count; ++i)
	{
		const uint8_t* raw = m_pendingRawBlocks.data() + i * _ZARCHIVE::COMPRESSED_BLOCK_SIZE;
		EmitCompressedBlock(raw, m_batchCompressed[i].data(), m_batchSizes[i]);
	}
	m_pendingBlockCount = 0;
}

void ZArchiveWriter::AppendData(const void* data, size_t size)
{
	size_t dataSize = size;
	const uint8_t* input = (const uint8_t*)data;
	while (size > 0)
	{
		size_t bytesToCopy = _ZARCHIVE::COMPRESSED_BLOCK_SIZE - m_currentWriteBuffer.size();
		if (bytesToCopy > size)
			bytesToCopy = size;
		if (bytesToCopy == _ZARCHIVE::COMPRESSED_BLOCK_SIZE)
		{
			EnqueueBlock(input);
			input += bytesToCopy;
			size -= bytesToCopy;
			continue;
		}
		m_currentWriteBuffer.insert(m_currentWriteBuffer.end(), input, input + bytesToCopy);
		input += bytesToCopy;
		size -= bytesToCopy;
		if (m_currentWriteBuffer.size() == _ZARCHIVE::COMPRESSED_BLOCK_SIZE)
		{
			EnqueueBlock(m_currentWriteBuffer.data());
			m_currentWriteBuffer.clear();
		}
	}
	if (m_currentFileNode)
		m_currentFileNode->fileSize += dataSize;
	m_currentInputOffset += dataSize;
}

void ZArchiveWriter::Finalize()
{
	m_currentFileNode = nullptr;
	if (!m_currentWriteBuffer.empty())
	{
		std::vector<uint8_t> padBuffer(_ZARCHIVE::COMPRESSED_BLOCK_SIZE - m_currentWriteBuffer.size(), 0);
		AppendData(padBuffer.data(), padBuffer.size());
	}
	FlushPendingBlocks();
	ShutdownWorkers();

	m_footer.sectionCompressedData.offset = 0;
	m_footer.sectionCompressedData.size = GetCurrentOutputOffset();
	while ((GetCurrentOutputOffset() % 8) != 0)
	{
		uint8_t b = 0;
		OutputData(&b, sizeof(uint8_t));
	}
	WriteOffsetRecords();
	WriteNameTable();
	WriteFileTree();
	WriteMetaData();
	WriteFooter();
}

void ZArchiveWriter::WriteOffsetRecords()
{
	m_footer.sectionOffsetRecords.offset = GetCurrentOutputOffset();
	_ZARCHIVE::CompressionOffsetRecord::Serialize(m_compressionOffsetRecord.data(), m_compressionOffsetRecord.size(), m_compressionOffsetRecord.data());
	OutputData(m_compressionOffsetRecord.data(), m_compressionOffsetRecord.size() * sizeof(_ZARCHIVE::CompressionOffsetRecord));
	m_footer.sectionOffsetRecords.size = GetCurrentOutputOffset() - m_footer.sectionOffsetRecords.offset;
}

void ZArchiveWriter::WriteNameTable()
{
	m_footer.sectionNames.offset = GetCurrentOutputOffset();
	uint32_t currentNameTableOffset = 0;
	m_nodeNameOffsets.resize(m_nodeNames.size());
	for (size_t i = 0; i < m_nodeNames.size(); i++)
	{
		m_nodeNameOffsets[i] = currentNameTableOffset;
		std::string_view name = m_nodeNames[i];
		if (name.size() > 0x7FFF)
			name = name.substr(0, 0x7FFF);
		if (name.size() >= 0x80)
		{
			uint8_t header[2];
			header[0] = (uint8_t)(name.size() & 0x7F) | 0x80;
			header[1] = (uint8_t)(name.size() >> 7);
			OutputData(header, 2);
			currentNameTableOffset += 2;
		}
		else
		{
			uint8_t header[1];
			header[0] = (uint8_t)name.size() & 0x7F;
			OutputData(header, 1);
			currentNameTableOffset += 1;
		}
		OutputData(name.data(), name.size());
		currentNameTableOffset += (uint32_t)name.size();
	}
	m_footer.sectionNames.size = GetCurrentOutputOffset() - m_footer.sectionNames.offset;
}

void ZArchiveWriter::WriteFileTree()
{
	std::queue<PathNode*> nodeQueue;
	nodeQueue.push(&m_rootNode);
	uint32_t currentIndex = 1;
	while (!nodeQueue.empty())
	{
		PathNode* node = nodeQueue.front();
		nodeQueue.pop();
		if (node->isFile)
		{
			node->nodeStartIndex = (uint32_t)0xFFFFFFFF;
			continue;
		}
		std::sort(node->subnodes.begin(), node->subnodes.end(),
			[&](ZArchiveWriter::PathNode*& a, ZArchiveWriter::PathNode*& b) -> int
			{
				return _ZARCHIVE::CompareNodeName(m_nodeNames[a->nameIndex], m_nodeNames[b->nameIndex]) > 0;
			});

		node->nodeStartIndex = currentIndex;
		currentIndex += (uint32_t)node->subnodes.size();
		for (auto& it : node->subnodes)
			nodeQueue.push(it);
	}

	m_footer.sectionFileTree.offset = GetCurrentOutputOffset();
	nodeQueue.push(&m_rootNode);
	while (!nodeQueue.empty())
	{
		PathNode* node = nodeQueue.front();
		nodeQueue.pop();

		_ZARCHIVE::FileDirectoryEntry tmp;
		if (node == &m_rootNode)
			tmp.SetTypeAndNameOffset(node->isFile, 0x7FFFFFFF);
		else
			tmp.SetTypeAndNameOffset(node->isFile, m_nodeNameOffsets[node->nameIndex]);
		if (node->isFile)
		{
			tmp.SetFileOffset(node->fileOffset);
			tmp.SetFileSize(node->fileSize);
		}
		else
		{
			tmp.directoryRecord.count = (uint32_t)node->subnodes.size();
			tmp.directoryRecord.nodeStartIndex = node->nodeStartIndex;
			tmp.directoryRecord._reserved = 0;
		}
		_ZARCHIVE::FileDirectoryEntry::Serialize(&tmp, 1, &tmp);
		OutputData(&tmp, sizeof(_ZARCHIVE::FileDirectoryEntry));
		for (auto& it : node->subnodes)
			nodeQueue.push(it);
	}
	m_footer.sectionFileTree.size = GetCurrentOutputOffset() - m_footer.sectionFileTree.offset;
}

void ZArchiveWriter::WriteMetaData()
{
	m_footer.sectionMetaDirectory.offset = GetCurrentOutputOffset();
	m_footer.sectionMetaDirectory.size = 0;
	m_footer.sectionMetaData.offset = GetCurrentOutputOffset();
	m_footer.sectionMetaData.size = 0;
}

void ZArchiveWriter::WriteFooter()
{
	m_footer.magic = _ZARCHIVE::Footer::kMagic;
	m_footer.version = _ZARCHIVE::Footer::kVersion1;
	m_footer.totalSize = GetCurrentOutputOffset() + sizeof(_ZARCHIVE::Footer);

	_ZARCHIVE::Footer tmp;

	memset(m_footer.integrityHash, 0, 32);
	_ZARCHIVE::Footer::Serialize(&m_footer, &tmp);
	sha_256_write(m_mainShaCtx, &tmp, sizeof(_ZARCHIVE::Footer));
	sha_256_close(m_mainShaCtx);
	free(m_mainShaCtx);
	m_mainShaCtx = nullptr;

	memcpy(m_footer.integrityHash, m_integritySha, 32);
	_ZARCHIVE::Footer::Serialize(&m_footer, &tmp);
	OutputData(&tmp, sizeof(_ZARCHIVE::Footer));
}
