#include <dam/search/vector_index.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>

#ifdef DAM_HAS_VECTOR_SEARCH
#include <hnswlib/hnswlib.h>
#endif

namespace dam::search {

// ============================================================================
// VectorIndex Implementation
// ============================================================================

VectorIndex::VectorIndex(VectorIndexConfig config)
    : config_(std::move(config)) {
}

VectorIndex::~VectorIndex() {
    cleanup();
}

VectorIndex::VectorIndex(VectorIndex&& other) noexcept
    : config_(std::move(other.config_))
    , hnsw_index_(other.hnsw_index_)
    , initialized_(other.initialized_) {
    other.hnsw_index_ = nullptr;
    other.initialized_ = false;
}

VectorIndex& VectorIndex::operator=(VectorIndex&& other) noexcept {
    if (this != &other) {
        cleanup();
        config_ = std::move(other.config_);
        hnsw_index_ = other.hnsw_index_;
        initialized_ = other.initialized_;
        other.hnsw_index_ = nullptr;
        other.initialized_ = false;
    }
    return *this;
}

void VectorIndex::cleanup() {
#ifdef DAM_HAS_VECTOR_SEARCH
    if (hnsw_index_) {
        auto* index = static_cast<hnswlib::HierarchicalNSW<float>*>(hnsw_index_);
        delete index;
        hnsw_index_ = nullptr;
    }
#endif
    initialized_ = false;
}

Result<void> VectorIndex::initialize() {
#ifndef DAM_HAS_VECTOR_SEARCH
    return Error(ErrorCode::NOT_FOUND,
        "Vector search not enabled. Rebuild with DAM_ENABLE_VECTOR_SEARCH=ON");
#else
    if (initialized_) {
        return {};
    }

    if (config_.dimension <= 0) {
        return Error(ErrorCode::INVALID_ARGUMENT, "Invalid dimension");
    }

    try {
        hnswlib::SpaceInterface<float>* space = nullptr;

        if (config_.distance_metric == "l2") {
            space = new hnswlib::L2Space(config_.dimension);
        } else if (config_.distance_metric == "ip" ||
                   config_.distance_metric == "cosine") {
            // Use inner product space for cosine similarity
            // (vectors must be normalized)
            space = new hnswlib::InnerProductSpace(config_.dimension);
        } else {
            return Error(ErrorCode::INVALID_ARGUMENT,
                "Unknown distance metric: " + config_.distance_metric);
        }

        auto* index = new hnswlib::HierarchicalNSW<float>(
            space,
            config_.max_elements,
            config_.M,
            config_.ef_construction,
            /* random_seed */ 42,
            config_.allow_replace);

        index->setEf(config_.ef_search);
        hnsw_index_ = index;
        initialized_ = true;

        return {};
    } catch (const std::exception& e) {
        return Error(ErrorCode::INTERNAL_ERROR,
            std::string("Failed to initialize HNSW index: ") + e.what());
    }
#endif
}

Result<void> VectorIndex::save(const std::string& path) const {
#ifndef DAM_HAS_VECTOR_SEARCH
    return Error(ErrorCode::NOT_FOUND, "Vector search not enabled");
#else
    if (!initialized_) {
        return Error(ErrorCode::INVALID_ARGUMENT, "Index not initialized");
    }

    std::string save_path = path.empty() ? config_.index_path : path;
    if (save_path.empty()) {
        return Error(ErrorCode::INVALID_ARGUMENT, "No save path specified");
    }

    try {
        auto* index = static_cast<hnswlib::HierarchicalNSW<float>*>(hnsw_index_);
        index->saveIndex(save_path);
        return {};
    } catch (const std::exception& e) {
        return Error(ErrorCode::IO_ERROR,
            std::string("Failed to save index: ") + e.what());
    }
#endif
}

Result<void> VectorIndex::load(const std::string& path) {
#ifndef DAM_HAS_VECTOR_SEARCH
    return Error(ErrorCode::NOT_FOUND, "Vector search not enabled");
#else
    std::string load_path = path.empty() ? config_.index_path : path;
    if (load_path.empty()) {
        return Error(ErrorCode::INVALID_ARGUMENT, "No load path specified");
    }

    // Check if file exists
    std::ifstream f(load_path);
    if (!f.good()) {
        return Error(ErrorCode::NOT_FOUND, "Index file not found: " + load_path);
    }
    f.close();

    try {
        cleanup();

        hnswlib::SpaceInterface<float>* space = nullptr;

        if (config_.distance_metric == "l2") {
            space = new hnswlib::L2Space(config_.dimension);
        } else {
            space = new hnswlib::InnerProductSpace(config_.dimension);
        }

        auto* index = new hnswlib::HierarchicalNSW<float>(
            space, load_path, false, config_.max_elements, config_.allow_replace);

        index->setEf(config_.ef_search);
        hnsw_index_ = index;
        initialized_ = true;

        return {};
    } catch (const std::exception& e) {
        return Error(ErrorCode::IO_ERROR,
            std::string("Failed to load index: ") + e.what());
    }
#endif
}

void VectorIndex::clear() {
#ifdef DAM_HAS_VECTOR_SEARCH
    if (initialized_) {
        // Recreate the index
        cleanup();
        initialize();
    }
#endif
}

Result<void> VectorIndex::resize(size_t new_max_elements) {
#ifndef DAM_HAS_VECTOR_SEARCH
    return Error(ErrorCode::NOT_FOUND, "Vector search not enabled");
#else
    if (!initialized_) {
        return Error(ErrorCode::INVALID_ARGUMENT, "Index not initialized");
    }

    try {
        auto* index = static_cast<hnswlib::HierarchicalNSW<float>*>(hnsw_index_);
        index->resizeIndex(new_max_elements);
        config_.max_elements = new_max_elements;
        return {};
    } catch (const std::exception& e) {
        return Error(ErrorCode::INTERNAL_ERROR,
            std::string("Failed to resize index: ") + e.what());
    }
#endif
}

Result<void> VectorIndex::add(FileId doc_id, const Embedding& embedding) {
#ifndef DAM_HAS_VECTOR_SEARCH
    return Error(ErrorCode::NOT_FOUND, "Vector search not enabled");
#else
    if (!initialized_) {
        return Error(ErrorCode::INVALID_ARGUMENT, "Index not initialized");
    }

    if (static_cast<int>(embedding.size()) != config_.dimension) {
        return Error(ErrorCode::INVALID_ARGUMENT,
            "Embedding dimension mismatch: expected " +
            std::to_string(config_.dimension) + ", got " +
            std::to_string(embedding.size()));
    }

    try {
        auto* index = static_cast<hnswlib::HierarchicalNSW<float>*>(hnsw_index_);

        // For cosine similarity, normalize the vector
        if (config_.distance_metric == "cosine") {
            Embedding normalized = embedding;
            Embedder::normalize(normalized);
            index->addPoint(normalized.data(), static_cast<hnswlib::labeltype>(doc_id));
        } else {
            index->addPoint(embedding.data(), static_cast<hnswlib::labeltype>(doc_id));
        }

        return {};
    } catch (const std::exception& e) {
        return Error(ErrorCode::INTERNAL_ERROR,
            std::string("Failed to add vector: ") + e.what());
    }
#endif
}

Result<void> VectorIndex::add_batch(
    const std::vector<FileId>& doc_ids,
    const std::vector<Embedding>& embeddings,
    EmbedProgressCallback callback) {

    if (doc_ids.size() != embeddings.size()) {
        return Error(ErrorCode::INVALID_ARGUMENT,
            "doc_ids and embeddings size mismatch");
    }

    for (size_t i = 0; i < doc_ids.size(); ++i) {
        auto result = add(doc_ids[i], embeddings[i]);
        if (!result.ok()) {
            return result;
        }

        if (callback) {
            callback(i + 1, doc_ids.size());
        }
    }

    return {};
}

Result<void> VectorIndex::remove(FileId doc_id) {
#ifndef DAM_HAS_VECTOR_SEARCH
    return Error(ErrorCode::NOT_FOUND, "Vector search not enabled");
#else
    if (!initialized_) {
        return Error(ErrorCode::INVALID_ARGUMENT, "Index not initialized");
    }

    try {
        auto* index = static_cast<hnswlib::HierarchicalNSW<float>*>(hnsw_index_);
        index->markDelete(static_cast<hnswlib::labeltype>(doc_id));
        return {};
    } catch (const std::exception& e) {
        return Error(ErrorCode::INTERNAL_ERROR,
            std::string("Failed to remove vector: ") + e.what());
    }
#endif
}

Result<void> VectorIndex::update(FileId doc_id, const Embedding& embedding) {
    // hnswlib supports replace if allow_replace is true
    return add(doc_id, embedding);
}

bool VectorIndex::contains(FileId doc_id) const {
#ifndef DAM_HAS_VECTOR_SEARCH
    return false;
#else
    if (!initialized_) {
        return false;
    }

    // hnswlib doesn't have a direct contains check, so we search
    // This is inefficient; in production, maintain a separate set
    auto* index = static_cast<hnswlib::HierarchicalNSW<float>*>(hnsw_index_);
    try {
        // Try to get the data for this label
        auto data = index->getDataByLabel<float>(static_cast<hnswlib::labeltype>(doc_id));
        return !data.empty();
    } catch (...) {
        return false;
    }
#endif
}

float VectorIndex::distance_to_similarity(float distance) const {
    if (config_.distance_metric == "cosine" || config_.distance_metric == "ip") {
        // Inner product space: higher = more similar
        // hnswlib uses (1 - cosine_similarity) for IP space with normalized vectors
        return 1.0f - distance;
    } else {
        // L2 distance: convert to similarity
        // Use exponential decay: sim = exp(-distance)
        return std::exp(-distance);
    }
}

Result<std::vector<VectorSearchResult>> VectorIndex::search(
    const Embedding& query,
    size_t k) const {
#ifndef DAM_HAS_VECTOR_SEARCH
    return Error(ErrorCode::NOT_FOUND, "Vector search not enabled");
#else
    if (!initialized_) {
        return Error(ErrorCode::INVALID_ARGUMENT, "Index not initialized");
    }

    if (static_cast<int>(query.size()) != config_.dimension) {
        return Error(ErrorCode::INVALID_ARGUMENT, "Query dimension mismatch");
    }

    auto* index = static_cast<hnswlib::HierarchicalNSW<float>*>(hnsw_index_);

    size_t num_elements = index->getCurrentElementCount();
    if (num_elements == 0) {
        return std::vector<VectorSearchResult>{};
    }

    k = std::min(k, num_elements);

    try {
        const float* query_data = query.data();

        // For cosine similarity, normalize query (use local buffer for thread safety)
        std::vector<float> normalized_query;
        if (config_.distance_metric == "cosine") {
            normalized_query.assign(query.begin(), query.end());
            Embedder::normalize(normalized_query);
            query_data = normalized_query.data();
        }

        auto result = index->searchKnn(query_data, k);

        std::vector<VectorSearchResult> results;
        results.reserve(result.size());

        while (!result.empty()) {
            auto [distance, label] = result.top();
            result.pop();

            VectorSearchResult r;
            r.doc_id = static_cast<FileId>(label);
            r.distance = distance;
            r.similarity = distance_to_similarity(distance);
            results.push_back(r);
        }

        // Results come out in reverse order (worst first)
        std::reverse(results.begin(), results.end());

        return results;
    } catch (const std::exception& e) {
        return Error(ErrorCode::INTERNAL_ERROR,
            std::string("Search failed: ") + e.what());
    }
#endif
}

Result<std::vector<VectorSearchResult>> VectorIndex::search_threshold(
    const Embedding& query,
    float max_distance,
    size_t k) const {

    auto result = search(query, k);
    if (!result.ok()) {
        return result;
    }

    auto& results = result.value();
    results.erase(
        std::remove_if(results.begin(), results.end(),
            [max_distance](const VectorSearchResult& r) {
                return r.distance > max_distance;
            }),
        results.end());

    return results;
}

Result<std::vector<VectorSearchResult>> VectorIndex::search_similarity(
    const Embedding& query,
    float min_similarity,
    size_t k) const {

    auto result = search(query, k);
    if (!result.ok()) {
        return result;
    }

    auto& results = result.value();
    results.erase(
        std::remove_if(results.begin(), results.end(),
            [min_similarity](const VectorSearchResult& r) {
                return r.similarity < min_similarity;
            }),
        results.end());

    return results;
}

void VectorIndex::set_ef_search(size_t ef_search) {
#ifdef DAM_HAS_VECTOR_SEARCH
    if (initialized_) {
        auto* index = static_cast<hnswlib::HierarchicalNSW<float>*>(hnsw_index_);
        index->setEf(ef_search);
        config_.ef_search = ef_search;
    }
#else
    (void)ef_search;
#endif
}

size_t VectorIndex::size() const {
#ifdef DAM_HAS_VECTOR_SEARCH
    if (initialized_) {
        auto* index = static_cast<hnswlib::HierarchicalNSW<float>*>(hnsw_index_);
        return index->getCurrentElementCount();
    }
#endif
    return 0;
}

Result<std::unique_ptr<VectorIndex>> VectorIndex::create_with_embedder(
    Embedder* embedder,
    VectorIndexConfig config) {

    if (!embedder) {
        return Error(ErrorCode::INVALID_ARGUMENT, "Embedder is required");
    }

    config.dimension = embedder->dimension();

    auto index = std::make_unique<VectorIndex>(std::move(config));
    auto init_result = index->initialize();
    if (!init_result.ok()) {
        return init_result.error();
    }

    return index;
}

// ============================================================================
// VectorIndexWithEmbedder Implementation
// ============================================================================

VectorIndexWithEmbedder::VectorIndexWithEmbedder(
    std::unique_ptr<VectorIndex> index,
    std::unique_ptr<Embedder> embedder)
    : index_(std::move(index))
    , embedder_(std::move(embedder)) {}

Result<void> VectorIndexWithEmbedder::index_text(FileId doc_id, const std::string& text) {
    auto embedding_result = embedder_->embed(text);
    if (!embedding_result.ok()) {
        return embedding_result.error();
    }

    return index_->add(doc_id, embedding_result.value());
}

Result<void> VectorIndexWithEmbedder::index_batch(
    const std::vector<FileId>& doc_ids,
    const std::vector<std::string>& texts,
    EmbedProgressCallback callback) {

    if (doc_ids.size() != texts.size()) {
        return Error(ErrorCode::INVALID_ARGUMENT, "Size mismatch");
    }

    auto embeddings_result = embedder_->embed_batch(texts, callback);
    if (!embeddings_result.ok()) {
        return embeddings_result.error();
    }

    return index_->add_batch(doc_ids, embeddings_result.value());
}

Result<std::vector<VectorSearchResult>> VectorIndexWithEmbedder::search(
    const std::string& query,
    size_t k) const {

    auto embedding_result = embedder_->embed(query);
    if (!embedding_result.ok()) {
        return embedding_result.error();
    }

    return index_->search(embedding_result.value(), k);
}

Result<std::vector<VectorSearchResult>> VectorIndexWithEmbedder::search_similar(
    const std::string& query,
    float min_similarity,
    size_t k) const {

    auto embedding_result = embedder_->embed(query);
    if (!embedding_result.ok()) {
        return embedding_result.error();
    }

    return index_->search_similarity(embedding_result.value(), min_similarity, k);
}

Result<std::unique_ptr<VectorIndexWithEmbedder>> VectorIndexWithEmbedder::create_from_env(
    VectorIndexConfig config) {

    auto embedder_result = EmbedderFactory::create_from_env();
    if (!embedder_result.ok()) {
        return embedder_result.error();
    }

    auto embedder = std::move(embedder_result.value());
    config.dimension = embedder->dimension();

    auto index = std::make_unique<VectorIndex>(std::move(config));
    auto init_result = index->initialize();
    if (!init_result.ok()) {
        return init_result.error();
    }

    return std::make_unique<VectorIndexWithEmbedder>(
        std::move(index), std::move(embedder));
}

}  // namespace dam::search
