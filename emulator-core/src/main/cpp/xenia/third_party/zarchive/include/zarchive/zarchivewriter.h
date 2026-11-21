#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "zarchivecommon.h"

struct ZSTD_CCtx_s;

class ZArchiveWriter
{
	struct PathNode
	{
		PathNode() : isFile(false), nameIndex(0xFFFFFFFF) {}
		PathNode(bool isFile, uint32_t nameIndex) : isFile(isFile), nameIndex(nameIndex) {}

		bool isFile;
		uint32_t nameIndex; // index in m_nodeNames

		std::vector<PathNode*> subnodes;

		// file properties
		uint64_t fileOffset{};
		uint64_t fileSize{};
		// directory properties
		uint32_t nodeStartIndex{};
	};

public:
	static constexpr size_t kBatchBlocks = 64; // 64 * 64 KiB = 4 MiB parallel batch

	typedef void(*CB_NewOutputFile)(const int32_t partIndex, void* ctx);
	typedef void(*CB_WriteOutputData)(const void* data, size_t length, void* ctx);

	ZArchiveWriter(CB_NewOutputFile cbNewOutputFile, CB_WriteOutputData cbWriteOutputData, void* ctx);
	~ZArchiveWriter();

	bool StartNewFile(const char* path); // creates a new virtual file and makes it active
	void AppendData(const void* data, size_t size); // appends data to currently active file
	bool MakeDir(const char* path, bool recursive = false);
	void Finalize();

private:
	PathNode* GetNodeByPath(PathNode* root, std::string_view path);
	PathNode* FindSubnodeByName(PathNode* parent, std::string_view nodeName);
	static void DestroySubtree(PathNode* node);

	uint32_t CreateNameEntry(std::string_view name);

	void OutputData(const void* data, size_t length);
	uint64_t GetCurrentOutputOffset() const;

	void EnqueueBlock(const uint8_t* uncompressedData);
	void FlushPendingBlocks();
	void CompressBatchRange(ZSTD_CCtx_s* cctx, size_t batchCount);
	void EmitCompressedBlock(const uint8_t* rawBlock, const uint8_t* compressedData, size_t compressedSize);
	void WorkerLoop();
	void ShutdownWorkers();

	void WriteOffsetRecords();
	void WriteNameTable();
	void WriteFileTree();
	void WriteMetaData();
	void WriteFooter();

private:
	// callbacks
	CB_NewOutputFile m_cbNewOutputFile;
	CB_WriteOutputData m_cbWriteOutputData;
	void* m_cbCtx;
	// file tree
	PathNode m_rootNode;
	PathNode* m_currentFileNode{ nullptr };
	std::vector<std::string> m_nodeNames;
	std::vector<uint32_t> m_nodeNameOffsets;
	std::unordered_map<std::string, uint32_t> m_nodeNameLookup;
	// footer
	_ZARCHIVE::Footer m_footer;
	// writes and parallel block compression
	std::vector<uint8_t> m_currentWriteBuffer;
	std::vector<uint8_t> m_pendingRawBlocks;
	std::array<std::vector<uint8_t>, kBatchBlocks> m_batchCompressed;
	std::array<size_t, kBatchBlocks> m_batchSizes{};
	size_t m_pendingBlockCount{ 0 };
	ZSTD_CCtx_s* m_inlineCctx{ nullptr };

	std::vector<std::thread> m_workers;
	std::mutex m_batchMutex;
	std::condition_variable m_batchStartCv;
	std::condition_variable m_batchDoneCv;
	uint64_t m_batchEpoch{ 0 };
	size_t m_activeBatchCount{ 0 };
	std::atomic<size_t> m_nextBlockIndex{ 0 };
	std::atomic<size_t> m_completedBlocks{ 0 };
	bool m_workersQuit{ false };

	uint64_t m_currentCompressedWriteIndex{ 0 }; // output file write index
	uint64_t m_currentInputOffset{ 0 }; // current offset within uncompressed file data
	// uncompressed-to-compressed offset records
	uint64_t m_numWrittenOffsetRecords{ 0 };
	std::vector<_ZARCHIVE::CompressionOffsetRecord> m_compressionOffsetRecord;
	// hashing
	struct Sha_256* m_mainShaCtx{};
	uint8_t m_integritySha[32];
};
