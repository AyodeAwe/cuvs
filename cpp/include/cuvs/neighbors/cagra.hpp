/*
 * Copyright (c) 2023-2024, NVIDIA CORPORATION.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "ann_types.hpp"
#include <cuvs/distance/distance_types.hpp>
#include <raft/core/device_mdspan.hpp>
#include <raft/core/host_device_accessor.hpp>
#include <raft/core/mdspan.hpp>
#include <raft/core/resources.hpp>
#include <raft/neighbors/cagra_types.hpp>
#include <rmm/cuda_stream_view.hpp>

namespace cuvs::neighbors::cagra {
/**
 * @addtogroup cagra
 * @{
 */

/**
 * @brief ANN algorithm used by CAGRA to build knn graph
 *
 */
enum class graph_build_algo {
  /* Use IVF-PQ to build all-neighbors knn graph */
  IVF_PQ,
  /* Experimental, use NN-Descent to build all-neighbors knn graph */
  NN_DESCENT
};

struct index_params : ann::index_params {
  /** Degree of input graph for pruning. */
  size_t intermediate_graph_degree = 128;
  /** Degree of output graph. */
  size_t graph_degree = 64;
  /** ANN algorithm to build knn graph. */
  graph_build_algo build_algo = graph_build_algo::IVF_PQ;
  /** Number of Iterations to run if building with NN_DESCENT */
  size_t nn_descent_niter = 20;

  /** Build a raft CAGRA index params from an existing cuvs CAGRA index params. */
  operator raft::neighbors::cagra::index_params() const
  {
    return raft::neighbors::cagra::index_params{
      {
        .metric            = static_cast<raft::distance::DistanceType>((int)this->metric),
        .metric_arg        = this->metric_arg,
        .add_data_on_build = this->add_data_on_build,
      },
      .intermediate_graph_degree = intermediate_graph_degree,
      .graph_degree              = graph_degree,
      .build_algo       = static_cast<raft::neighbors::cagra::graph_build_algo>((int)build_algo),
      .nn_descent_niter = nn_descent_niter};
  }
};

enum class search_algo {
  /** For large batch sizes. */
  SINGLE_CTA,
  /** For small batch sizes. */
  MULTI_CTA,
  MULTI_KERNEL,
  AUTO
};

enum class hash_mode { HASH, SMALL, AUTO };

struct search_params : ann::search_params {
  /** Maximum number of queries to search at the same time (batch size). Auto select when 0.*/
  size_t max_queries = 0;

  /** Number of intermediate search results retained during the search.
   *
   *  This is the main knob to adjust trade off between accuracy and search speed.
   *  Higher values improve the search accuracy.
   */
  size_t itopk_size = 64;

  /** Upper limit of search iterations. Auto select when 0.*/
  size_t max_iterations = 0;

  // In the following we list additional search parameters for fine tuning.
  // Reasonable default values are automatically chosen.

  /** Which search implementation to use. */
  search_algo algo = search_algo::AUTO;

  /** Number of threads used to calculate a single distance. 4, 8, 16, or 32. */
  size_t team_size = 0;

  /** Number of graph nodes to select as the starting point for the search in each iteration. aka
   * search width?*/
  size_t search_width = 1;
  /** Lower limit of search iterations. */
  size_t min_iterations = 0;

  /** Thread block size. 0, 64, 128, 256, 512, 1024. Auto selection when 0. */
  size_t thread_block_size = 0;
  /** Hashmap type. Auto selection when AUTO. */
  hash_mode hashmap_mode = hash_mode::AUTO;
  /** Lower limit of hashmap bit length. More than 8. */
  size_t hashmap_min_bitlen = 0;
  /** Upper limit of hashmap fill rate. More than 0.1, less than 0.9.*/
  float hashmap_max_fill_rate = 0.5;

  /** Number of iterations of initial random seed node selection. 1 or more. */
  uint32_t num_random_samplings = 1;
  /** Bit mask used for initial random seed node selection. */
  uint64_t rand_xor_mask = 0x128394;

  /** Build a raft CAGRA search params from an existing cuvs CAGRA search params. */
  operator raft::neighbors::cagra::search_params() const
  {
    raft::neighbors::cagra::search_params result = {
      {},
      max_queries,
      itopk_size,
      max_iterations,
      static_cast<raft::neighbors::cagra::search_algo>((int)algo),
      team_size,
      search_width,
      min_iterations,
      thread_block_size,
      static_cast<raft::neighbors::cagra::hash_mode>((int)hashmap_mode),
      hashmap_min_bitlen,
      hashmap_max_fill_rate,
      num_random_samplings,
      rand_xor_mask};
    return result;
  }
};

static_assert(std::is_aggregate_v<index_params>);
static_assert(std::is_aggregate_v<search_params>);

/**
 * @brief CAGRA index.
 *
 * The index stores the dataset and a kNN graph in device memory.
 *
 * @tparam T data element type
 * @tparam IdxT type of the vector indices (represent dataset.extent(0))
 *
 */
template <typename T, typename IdxT>
struct index : ann::index {
  /** Build a cuvs CAGRA index from an existing RAFT CAGRA index. */
  index(raft::neighbors::cagra::index<T, IdxT>&& raft_idx)
    : ann::index(),
      raft_index_{std::make_unique<raft::neighbors::cagra::index<T, IdxT>>(std::move(raft_idx))}
  {
  }
  static_assert(!raft::is_narrowing_v<uint32_t, IdxT>,
                "IdxT must be able to represent all values of uint32_t");

 public:
  /** Distance metric used for clustering. */
  [[nodiscard]] constexpr inline auto metric() const noexcept -> cuvs::distance::DistanceType
  {
    return static_cast<cuvs::distance::DistanceType>((int)raft_index_->metric());
  }

  /** Total length of the index (number of vectors). */
  [[nodiscard]] constexpr inline auto size() const noexcept -> IdxT { return raft_index_->size(); }

  /** Dimensionality of the data. */
  [[nodiscard]] constexpr inline auto dim() const noexcept -> uint32_t
  {
    return raft_index_->dim();
  }
  /** Graph degree */
  [[nodiscard]] constexpr inline auto graph_degree() const noexcept -> uint32_t
  {
    return raft_index_->graph_degree();
  }

  /** Dataset [size, dim] */
  [[nodiscard]] inline auto dataset() const noexcept
    -> raft::device_matrix_view<const T, int64_t, raft::layout_stride>
  {
    return raft_index_->dataset();
  }

  /** neighborhood graph [size, graph-degree] */
  [[nodiscard]] inline auto graph() const noexcept
    -> raft::device_matrix_view<const IdxT, int64_t, raft::row_major>
  {
    return raft_index_->graph();
  }

  // Don't allow copying the index for performance reasons (try avoiding copying data)
  index(const index&)                    = delete;
  index(index&&)                         = default;
  auto operator=(const index&) -> index& = delete;
  auto operator=(index&&) -> index&      = default;
  ~index()                               = default;

  /** Construct an empty index. */
  index(raft::resources const& res,
        cuvs::distance::DistanceType metric = cuvs::distance::DistanceType::L2Expanded)
    : ann::index(),
      raft_index_(std::make_unique<raft::neighbors::cagra::index<T, IdxT>>(
        res, static_cast<raft::distance::DistanceType>((int)metric)))
  {
  }
  /** Construct an index from dataset and knn_graph arrays
   *
   * If the dataset and graph is already in GPU memory, then the index is just a thin wrapper around
   * these that stores a non-owning a reference to the arrays.
   *
   * The constructor also accepts host arrays. In that case they are copied to the device, and the
   * device arrays will be owned by the index.
   *
   * In case the dasates rows are not 16 bytes aligned, then we create a padded copy in device
   * memory to ensure alignment for vectorized load.
   *
   * Usage examples:
   *
   * - Cagra index is normally created by the cagra::build
   * @code{.cpp}
   *   using namespace cuvs::neighbors::experimental;
   *   auto dataset = raft::make_host_matrix<float, int64_t>(n_rows, n_cols);
   *   load_dataset(dataset.view());
   *   // use default index parameters
   *   cagra::index_params index_params;
   *   // create and fill the index from a [N, D] dataset
   *   auto index = cagra::build(res, index_params, dataset);
   *   // use default search parameters
   *   cagra::search_params search_params;
   *   // search K nearest neighbours
   *   auto neighbors = raft::make_device_matrix<uint32_t, int64_t>(res, n_queries, k);
   *   auto distances = raft::make_device_matrix<float, int64_t>(res, n_queries, k);
   *   cagra::search(res, search_params, index, queries, neighbors, distances);
   * @endcode
   *   In the above example, we have passed a host dataset to build. The returned index will own a
   * device copy of the dataset and the knn_graph. In contrast, if we pass the dataset as a
   * raft::device_mdspan to build, then it will only store a reference to it.
   *
   * - Constructing index using existing knn-graph
   * @code{.cpp}
   *   using namespace cuvs::neighbors::experimental;
   *
   *   auto dataset = raft::make_device_matrix<float, int64_t>(res, n_rows, n_cols);
   *   auto knn_graph = raft::make_device_matrix<uint32_n, int64_t>(res, n_rows, graph_degree);
   *
   *   // custom loading and graph creation
   *   // load_dataset(dataset.view());
   *   // create_knn_graph(knn_graph.view());
   *
   *   // Wrap the existing device arrays into an index structure
   *   cagra::index<T, IdxT> index(res, metric, raft::make_const_mdspan(dataset.view()),
   *                               raft::make_const_mdspan(knn_graph.view()));
   *
   *   // Both knn_graph and dataset objects have to be in scope while the index is used because
   *   // the index only stores a reference to these.
   *   cagra::search(res, search_params, index, queries, neighbors, distances);
   * @endcode
   *
   */
  template <typename data_accessor, typename graph_accessor>
  index(raft::resources const& res,
        cuvs::distance::DistanceType metric,
        raft::mdspan<const T, raft::matrix_extent<int64_t>, raft::row_major, data_accessor> dataset,
        raft::mdspan<const IdxT, raft::matrix_extent<int64_t>, raft::row_major, graph_accessor>
          knn_graph)
    : ann::index(),
      raft_index_(std::make_unique<raft::neighbors::cagra::index<T, IdxT>>(
        res, static_cast<raft::distance::DistanceType>((int)metric), dataset, knn_graph))
  {
    RAFT_EXPECTS(dataset.extent(0) == knn_graph.extent(0),
                 "Dataset and knn_graph must have equal number of rows");
    update_dataset(res, dataset);
    update_graph(res, knn_graph);
    raft::resource::sync_stream(res);
  }

  /**
   * Replace the dataset with a new dataset.
   *
   * If the new dataset rows are aligned on 16 bytes, then only a reference is stored to the
   * dataset. It is the caller's responsibility to ensure that dataset stays alive as long as the
   * index.
   */
  void update_dataset(raft::resources const& res,
                      raft::device_matrix_view<const T, int64_t, raft::row_major> dataset)
  {
    raft_index_->update_dataset(res, dataset);
  }
  /**
   * Replace the dataset with a new dataset.
   *
   * We create a copy of the dataset on the device. The index manages the lifetime of this copy.
   */
  void update_dataset(raft::resources const& res,
                      raft::host_matrix_view<const T, int64_t, raft::row_major> dataset)
  {
    raft_index_->update_dataset(res, dataset);
  }

  /**
   * Replace the graph with a new graph.
   *
   * Since the new graph is a device array, we store a reference to that, and it is
   * the caller's responsibility to ensure that knn_graph stays alive as long as the index.
   */
  void update_graph(raft::resources const& res,
                    raft::device_matrix_view<const IdxT, int64_t, raft::row_major> knn_graph)
  {
    raft_index_->update_graph(res, knn_graph);
  }

  /**
   * Replace the graph with a new graph.
   *
   * We create a copy of the graph on the device. The index manages the lifetime of this copy.
   */
  void update_graph(raft::resources const& res,
                    raft::host_matrix_view<const IdxT, int64_t, raft::row_major> knn_graph)
  {
    raft_index_->update_graph(res, knn_graph);
  }

  auto get_raft_index() const -> const raft::neighbors::cagra::index<T, IdxT>*
  {
    return raft_index_.get();
  }
  auto get_raft_index() -> raft::neighbors::cagra::index<T, IdxT>* { return raft_index_.get(); }

 private:
  std::unique_ptr<raft::neighbors::cagra::index<T, IdxT>> raft_index_;
};

// Using device and host_matrix_view avoids needing to typedef multiple mdspans based on accessors
#define CUVS_INST_CAGRA_FUNCS(T, IdxT)                                                   \
  auto build(raft::resources const& handle,                                              \
             const cuvs::neighbors::cagra::index_params& params,                         \
             raft::device_matrix_view<const T, int64_t, raft::row_major> dataset)        \
    ->cuvs::neighbors::cagra::index<T, IdxT>;                                            \
                                                                                         \
  auto build(raft::resources const& handle,                                              \
             const cuvs::neighbors::cagra::index_params& params,                         \
             raft::host_matrix_view<const T, int64_t, raft::row_major> dataset)          \
    ->cuvs::neighbors::cagra::index<T, IdxT>;                                            \
                                                                                         \
  void build_device(raft::resources const& handle,                                       \
                    const cuvs::neighbors::cagra::index_params& params,                  \
                    raft::device_matrix_view<const T, int64_t, raft::row_major> dataset, \
                    cuvs::neighbors::cagra::index<T, IdxT>& idx);                        \
                                                                                         \
  void build_host(raft::resources const& handle,                                         \
                  const cuvs::neighbors::cagra::index_params& params,                    \
                  raft::host_matrix_view<const T, int64_t, raft::row_major> dataset,     \
                  cuvs::neighbors::cagra::index<T, IdxT>& idx);                          \
                                                                                         \
  void search(raft::resources const& handle,                                             \
              cuvs::neighbors::cagra::search_params const& params,                       \
              const cuvs::neighbors::cagra::index<T, IdxT>& index,                       \
              raft::device_matrix_view<const T, int64_t, raft::row_major> queries,       \
              raft::device_matrix_view<IdxT, int64_t, raft::row_major> neighbors,        \
              raft::device_matrix_view<float, int64_t, raft::row_major> distances);      \
  void serialize_file(raft::resources const& handle,                                     \
                      const std::string& filename,                                       \
                      const cuvs::neighbors::cagra::index<T, IdxT>& index,               \
                      bool include_dataset = true);                                      \
                                                                                         \
  void deserialize_file(raft::resources const& handle,                                   \
                        const std::string& filename,                                     \
                        cuvs::neighbors::cagra::index<T, IdxT>* index);                  \
  void serialize(raft::resources const& handle,                                          \
                 std::string& str,                                                       \
                 const cuvs::neighbors::cagra::index<T, IdxT>& index,                    \
                 bool include_dataset = true);                                           \
                                                                                         \
  void deserialize(raft::resources const& handle,                                        \
                   const std::string& str,                                               \
                   cuvs::neighbors::cagra::index<T, IdxT>* index);

CUVS_INST_CAGRA_FUNCS(float, uint32_t);
CUVS_INST_CAGRA_FUNCS(int8_t, uint32_t);
CUVS_INST_CAGRA_FUNCS(uint8_t, uint32_t);

#undef CUVS_INST_CAGRA_FUNCS

#define CUVS_INST_CAGRA_OPTIMIZE(IdxT)                                                     \
  void optimize_device(raft::resources const& res,                                         \
                       raft::device_matrix_view<IdxT, int64_t, raft::row_major> knn_graph, \
                       raft::host_matrix_view<IdxT, int64_t, raft::row_major> new_graph);  \
                                                                                           \
  void optimize_host(raft::resources const& res,                                           \
                     raft::host_matrix_view<IdxT, int64_t, raft::row_major> knn_graph,     \
                     raft::host_matrix_view<IdxT, int64_t, raft::row_major> new_graph);

CUVS_INST_CAGRA_OPTIMIZE(uint32_t);

#undef CUVS_INST_CAGRA_OPTIMIZE

/** @} */

}  // namespace cuvs::neighbors::cagra
