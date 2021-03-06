#include <torch/csrc/jit/passes/tensorexpr_fuser.h>
#include <ATen/record_function.h>
#include <torch/csrc/jit/codegen/fuser/interface.h>
#include <torch/csrc/jit/ir/alias_analysis.h>
#include <torch/csrc/jit/jit_log.h>
#include <torch/csrc/jit/passes/common_subexpression_elimination.h>
#include <torch/csrc/jit/passes/dead_code_elimination.h>
#include <torch/csrc/jit/passes/pass_manager.h>
#include <torch/csrc/jit/passes/remove_redundant_profiles.h>
#include <torch/csrc/jit/passes/utils/subgraph_utils.h>
#include <torch/csrc/jit/runtime/custom_operator.h>
#include <torch/csrc/jit/runtime/graph_executor.h>
#include <torch/csrc/jit/runtime/operator_options.h>
#include <torch/csrc/jit/tensorexpr/kernel.h>
#include <torch/csrc/utils/memory.h>

namespace torch {
namespace jit {

static bool texpr_reductions_enabled = false;

bool isSupportedForBlock(Node* node) {
  switch (node->kind()) {
    case aten::add:
    case aten::mul:
      return true;
    default:
      return false;
  }
}

namespace tensorexpr {
bool isSupported(Node* node) {
  // For Block codegen we allow limited ops.
  if (tensorexpr::getTEGenerateBlockCode()) {
    return isSupportedForBlock(node);
  }

  // clang-format off
  // breaks up the schema strings so they are no longer discoverable with ctrl-F
  static const OperatorSet supported_operator_set{
      "aten::add.Tensor(Tensor self, Tensor other, *, Scalar alpha=1) -> Tensor",
      "aten::add.Scalar(Tensor self, Scalar other, Scalar alpha=1) -> Tensor",
      "aten::_cast_Float(Tensor self, bool non_blocking) -> Tensor",
      "aten::type_as(Tensor self, Tensor other) -> Tensor",
      "aten::sub.Tensor(Tensor self, Tensor other, *, Scalar alpha=1) -> Tensor",
      "aten::sub.Scalar(Tensor self, Scalar other, Scalar alpha=1) -> Tensor",
      "aten::mul.Tensor(Tensor self, Tensor other) -> Tensor",
      "aten::mul.Scalar(Tensor self, Scalar other) -> Tensor",
      "aten::div.Tensor(Tensor self, Tensor other) -> Tensor",
      "aten::div.Scalar(Tensor self, Scalar other) -> Tensor",
      "aten::eq.Tensor(Tensor self, Tensor other) -> Tensor",
      "aten::eq.Scalar(Tensor self, Scalar other) -> Tensor",
      "aten::ne.Tensor(Tensor self, Tensor other) -> Tensor",
      "aten::ne.Scalar(Tensor self, Scalar other) -> Tensor",
      "aten::ge.Tensor(Tensor self, Tensor other) -> Tensor",
      "aten::ge.Scalar(Tensor self, Scalar other) -> Tensor",
      "aten::gt.Tensor(Tensor self, Tensor other) -> Tensor",
      "aten::gt.Scalar(Tensor self, Scalar other) -> Tensor",
      "aten::le.Tensor(Tensor self, Tensor other) -> Tensor",
      "aten::le.Scalar(Tensor self, Scalar other) -> Tensor",
      "aten::lt.Tensor(Tensor self, Tensor other) -> Tensor",
      "aten::lt.Scalar(Tensor self, Scalar other) -> Tensor",
      "aten::pow.Tensor_Scalar(Tensor self, Scalar exponent) -> Tensor",
      "aten::pow.Tensor_Tensor(Tensor self, Tensor exponent) -> Tensor",
      // TODO : do we support pow.Scalar ?
      "aten::pow.Scalar(Scalar self, Tensor exponent) -> Tensor",
      // TODO: support clamp_min, clamp_max
      "aten::clamp(Tensor self, Scalar? min=None, Scalar? max=None) -> Tensor",
      "aten::lerp.Scalar(Tensor self, Tensor end, Scalar weight) -> Tensor",
      "aten::lerp.Tensor(Tensor self, Tensor end, Tensor weight) -> Tensor",
      "aten::log10(Tensor self) -> Tensor",
      "aten::log(Tensor self) -> Tensor",
      "aten::log2(Tensor self) -> Tensor",
      // TODO: log1p
      "aten::exp(Tensor self) -> Tensor",
      "aten::erf(Tensor self) -> Tensor",
      "aten::erfc(Tensor self) -> Tensor",
      "aten::fmod.Scalar(Tensor self, Scalar other) -> Tensor",
      "aten::fmod.Tensor(Tensor self, Tensor other) -> Tensor",
      "aten::cos(Tensor self) -> Tensor",
      "aten::sin(Tensor self) -> Tensor",
      "aten::tan(Tensor self) -> Tensor",
      "aten::acos(Tensor self) -> Tensor",
      "aten::asin(Tensor self) -> Tensor",
      "aten::atan(Tensor self) -> Tensor",
      "aten::atan2(Tensor self, Tensor other) -> Tensor",
      "aten::cosh(Tensor self) -> Tensor",
      "aten::sinh(Tensor self) -> Tensor",
      "aten::tanh(Tensor self) -> Tensor",
      "aten::sqrt(Tensor self) -> Tensor",
      "aten::rsqrt(Tensor self) -> Tensor",
      "aten::abs(Tensor self) -> Tensor",
      "aten::floor(Tensor self) -> Tensor",
      "aten::ceil(Tensor self) -> Tensor",
      "aten::round(Tensor self) -> Tensor",
      "aten::trunc(Tensor self) -> Tensor",
      "aten::threshold(Tensor self, Scalar threshold, Scalar value) -> Tensor",
      "aten::remainder.Scalar(Tensor self, Scalar other) -> Tensor",
      "aten::remainder.Tensor(Tensor self, Tensor other) -> Tensor",
      "aten::cat(Tensor[] tensors, int dim=0) -> Tensor",
      "aten::sigmoid(Tensor self) -> Tensor",
      "aten::relu(Tensor self) -> Tensor",
      "aten::addcmul(Tensor self, Tensor tensor1, Tensor tensor2, *, Scalar value=1) -> Tensor",
      "aten::neg(Tensor self) -> Tensor",
      "aten::reciprocal(Tensor self) -> Tensor",
      "aten::expm1(Tensor self) -> Tensor",
      "aten::unsqueeze(Tensor(a) self, int dim) -> Tensor(a)",
      "aten::frac(Tensor self) -> Tensor",
      // TODO: uncomment once we can handle rand+broadcasts
      // "aten::rand_like(Tensor self, *, ScalarType? dtype=None, Layout? layout=None, Device? device=None, bool? pin_memory=None, MemoryFormat? memory_format=None) -> Tensor",
      "aten::__and__.Scalar(Tensor self, Scalar other) -> Tensor",
      "aten::__and__.Tensor(Tensor self, Tensor other) -> Tensor",
      "aten::__or__.Scalar(Tensor self, Scalar other) -> Tensor",
      "aten::__or__.Tensor(Tensor self, Tensor other) -> Tensor",
      "aten::__xor__.Scalar(Tensor self, Scalar other) -> Tensor",
      "aten::__xor__.Tensor(Tensor self, Tensor other) -> Tensor",
      "aten::__lshift__.Scalar(Tensor self, Scalar other) -> Tensor",
      "aten::__lshift__.Tensor(Tensor self, Tensor other) -> Tensor",
      "aten::__rshift__.Scalar(Tensor self, Scalar other) -> Tensor",
      "aten::__rshift__.Tensor(Tensor self, Tensor other) -> Tensor",
      "aten::where.self(Tensor condition, Tensor self, Tensor other) -> Tensor",
      "aten::where.ScalarSelf(Tensor condition, Scalar self, Tensor other) -> Tensor",
      "aten::where.ScalarOther(Tensor condition, Tensor self, Scalar other) -> Tensor",
      "aten::where.Scalar(Tensor condition, Scalar self, Scalar other) -> Tensor",
      "aten::where(Tensor condition) -> Tensor[]",
      // TODO: enable other min/max variants, operators that can be both
      // elementwise or reductions:
      "aten::min.other(Tensor self, Tensor other) -> Tensor",
      "aten::max.other(Tensor self, Tensor other) -> Tensor",
      // TODO: enable slice, shape inference is not implemented for this op yet
  };
  static const OperatorSet supported_reduction_set{
      "aten::sum(Tensor self, *, ScalarType? dtype=None) -> Tensor",
      "aten::sum.dim_IntList(Tensor self, int[1] dim, bool keepdim=False, *, ScalarType? dtype=None) -> Tensor",
  };
  // clang-format on

  if (node->isMemberOf(supported_operator_set) ||
      (texpr_reductions_enabled && node->isMemberOf(supported_reduction_set))) {
    // We only insert guards on Tensor types, so we rely on the output
    // of a node being uniquely determined by its input types.
    // bail if any non-Tensor input affects the output type
    // and cannot be reasoned about statically

    // Value is either an int or a float (can occur from .item())
    for (Value* v : node->inputs()) {
      if (v->type()->cast<NumberType>()) {
        return false;
      }
    }

    // non-const dtype / device
    for (auto arg_name : {"dtype", "device"}) {
      if (auto index = node->schema().argumentIndexWithName(arg_name)) {
        if (!toIValue(node->input(*index))) {
          return false;
        }
      }
    }

    return true;
  }

  // unschematized ops
  switch (node->kind()) {
    case prim::ConstantChunk:
    case prim::ListConstruct:
      return true;
  }

  return false;
}

} // namespace tensorexpr

static bool texpr_fuser_enabled_ = false;

void setTensorExprFuserEnabled(bool val) {
  texpr_fuser_enabled_ = val;
}

bool tensorExprFuserEnabled() {
  static const char* enable_c_str = std::getenv("PYTORCH_TENSOREXPR");
  if (!enable_c_str) {
    return texpr_fuser_enabled_;
  }
  if (std::string(enable_c_str) == "0") {
    return false;
  }
  return true;
}

bool setTexprReductionsEnabled(bool value) {
  bool old_value = texpr_reductions_enabled;
  texpr_reductions_enabled = value;
  return old_value;
}

bool texprReductionsEnabled() {
  return texpr_reductions_enabled;
}

struct nodesComparator {
  bool operator()(Node* a, Node* b) const {
    return a->isAfter(b);
  }
};

// TODO: if a value has differently typed uses, temporarrily insert a node
// specializing the type for each use and later remove, instead of bailing
bool profiledWithDifferentTypes(Value* v) {
  std::vector<TypePtr> types;
  for (const auto& use : v->uses()) {
    if (use.user->kind() == prim::profile) {
      types.push_back(use.user->ty(attr::profiled_type));
    }
  }
  for (size_t i = 1; i < types.size(); ++i) {
    if (types.at(i - 1) != types.at(i)) {
      return true;
    }
  }
  return false;
}

void removeProfileNodesAndSpecializeTypes(Block* b) {
  for (auto it = b->nodes().begin(); it != b->nodes().end(); it++) {
    if (it->kind() == prim::profile) {
      GRAPH_DEBUG("Removing prim::profile: %", it->output()->debugName());
      it->output()->replaceAllUsesWith(it->input());
      if (!profiledWithDifferentTypes(it->input())) {
        it->input()->setType(it->ty(attr::profiled_type));
      } else {
        GRAPH_DEBUG(
            "Ignoring value with differently typed profiles :%",
            it->output()->debugName());
      }
      it.destroyCurrent();
    } else {
      for (Block* ib : it->blocks()) {
        removeProfileNodesAndSpecializeTypes(ib);
      }
    }
  }
}

void RemoveProfileNodesAndSpecializeTypes(std::shared_ptr<Graph>& graph) {
  removeProfileNodesAndSpecializeTypes(graph->block());
}

void removeTensorTypeSpecialization(Value* v) {
  if (!v->type()->cast<TensorType>()) {
    return;
  }
  // Constants & TensorExprGroup will always produce specialized tensor type,
  // TypeCheck are inserted by this pass and only used by fusion groups that
  // insert proper guards
  if (v->node()->kind() == prim::Constant ||
      v->node()->kind() == prim::TypeCheck ||
      v->node()->kind() == prim::TensorExprGroup) {
    return;
  }
  v->setType(TensorType::get());
}

void removeTensorTypeSpecializations(Block* block) {
  for (Value* v : block->inputs()) {
    removeTensorTypeSpecialization(v);
  }
  for (Node* n : block->nodes()) {
    for (Block* b : n->blocks()) {
      removeTensorTypeSpecializations(b);
    }
    for (Value* v : n->outputs()) {
      removeTensorTypeSpecialization(v);
    }
  }
}

void RemoveTensorTypeSpecializations(std::shared_ptr<Graph>& graph) {
  removeTensorTypeSpecializations(graph->block());
}

class TensorExprFuser {
 public:
  TensorExprFuser(std::shared_ptr<Graph> graph, size_t min_group_size)
      : graph_(std::move(graph)), min_group_size_(min_group_size) {}

  void run() {
    aliasDb_ = torch::make_unique<AliasDb>(graph_);
    RemoveRedundantProfiles(graph_);
    GRAPH_DUMP("After removing redundant profile nodes: ", graph_);
    createFusionGroups(graph_->block());
    GRAPH_DUMP("After creating fusion groups: ", graph_);
    guardFusionGroups(graph_->block());
    GRAPH_DUMP("After guarding fusion groups: ", graph_);
    removeTensorTypeSpecializations(graph_->block());
    GRAPH_DUMP("After removing tensor type specializations: ", graph_);
  }

 private:
  // Merges `to_merge` into a subgraph by executing merge_fn.
  // merge_fn takes in map that will be filled with the mapping b/w
  // to_merge's outputs and the corresponding values in the subgraph.
  // merge_fn returns the merged-into subgraph
  Node* aliasingSafeSubgraphMerge(
      Node* to_merge,
      const std::function<Node*(std::unordered_map<Value*, Value*>&)>&
          merge_fn) {
    // When we merge a node into a subgraph, the new subgraph outputs
    // have the same aliasing properties as the original node's outputs.
    // Here we create a placeholder node, transfer the aliasing properties
    // to the placeholder, execute the merge, and transfer the aliasing
    // properties to the appropriate fusion group outputs
    Node* placeholder_node =
        graph_->insertNode(graph_->create(prim::Uninitialized, 0));
    std::vector<Value*> existing_values;
    for (size_t i = 0; i < to_merge->outputs().size(); ++i) {
      Value* existing = to_merge->outputs().at(i);
      Value* new_value = placeholder_node->insertOutput(i)->copyMetadata(
          to_merge->outputs().at(i));
      aliasDb_->replaceWithNewValue(existing, new_value);
      existing_values.push_back(existing);
    }
    std::unordered_map<Value*, Value*> vmap;
    Node* fusion_group = merge_fn(vmap);
    for (size_t i = 0; i < existing_values.size(); ++i) {
      TORCH_INTERNAL_ASSERT(vmap.count(existing_values.at(i)));
      Value* subgraph_value = vmap[existing_values.at(i)];
      auto subgraph = SubgraphUtils::getSubgraph(fusion_group);
      size_t subgraph_output_index = 0;
      for (; subgraph_output_index < subgraph->outputs().size();
           ++subgraph_output_index) {
        if (subgraph->outputs().at(subgraph_output_index) == subgraph_value) {
          break;
        }
      }
      if (subgraph_output_index != subgraph->outputs().size()) {
        aliasDb_->replaceWithNewValue(
            placeholder_node->outputs().at(i),
            fusion_group->outputs().at(subgraph_output_index));
      }
    }
    placeholder_node->destroy();
    return fusion_group;
  }

  Node* getOrCreateTensorExprSubgraph(Node* n) {
    if (n->hasAttribute(attr::Subgraph) && n->kind() == prim::TensorExprGroup) {
      return n;
    }
    GRAPH_UPDATE("Creating a tensorexpr::Group node from: ", *n);
    return aliasingSafeSubgraphMerge(
        n, [&](std::unordered_map<Value*, Value*>& vmap) {
          return SubgraphUtils::createSingletonSubgraph(
              n, prim::TensorExprGroup, vmap);
        });
  }

  void mergeNodeIntoSubgraphAndUpdateAliasing(Node* n, Node* subgraph) {
    aliasingSafeSubgraphMerge(n, [&](std::unordered_map<Value*, Value*>& vmap) {
      SubgraphUtils::mergeNodeIntoSubgraph(n, subgraph, vmap);
      return subgraph;
    });
  }

  // Add unvisited input nodes to the queue for further merging into the fusion
  // group.
  void updateQueue(
      Node* fusion_group,
      std::set<Node*, nodesComparator>& queue,
      const std::unordered_set<Node*>& visited) {
    for (auto input : fusion_group->inputs()) {
      if (!visited.count(input->node())) {
        queue.insert(input->node());
      }
    }
  }

  // Create a fusion group starting from the node N.
  // We then try to pull inputs into the fusion group and repeat that process
  // until there is nothing we can pull in.
  Node* createFusionGroup(Node* n) {
    // Queue of the nodes we should consider for merging into the fusion groups
    // (those nodes are usually inputs of the fusion group).
    // We use an ordered set here to visit them in the right order: the fusion
    // group is closer to the end of the block and we are trying to pull later
    // nodes first.
    // NB: the order in the list in theory could stale if we move nodes around.
    // However, this should only happen to the nodes we could not fuse, and
    // hence it should not be a problem.
    std::set<Node*, nodesComparator> queue;
    std::unordered_set<Node*> visited_nodes;

    Node* fusion_group = n;
    if (min_group_size_ == 1) {
      fusion_group = getOrCreateTensorExprSubgraph(n);
    }

    updateQueue(fusion_group, queue, visited_nodes);

    GRAPH_DEBUG("Iteratively pull input nodes into the fusion group...\n");
    while (!queue.empty()) {
      debugDumpFusionGroup("Current fusion group: ", fusion_group);
      GRAPH_DEBUG(queue.size(), " nodes are in the queue.\n");

      Node* input_node = *queue.begin();
      queue.erase(queue.begin());

      GRAPH_DEBUG("Trying to merge: ", *input_node);
      fusion_group = tryMerge(fusion_group, input_node);
      visited_nodes.insert(input_node);
      updateQueue(fusion_group, queue, visited_nodes);
    }

    return fusion_group;
  }

  static void debugDumpFusionGroup(const std::string& msg, Node* n) {
    GRAPH_DEBUG(msg, *n);
    if (n->kind() == prim::TensorExprGroup) {
      GRAPH_DEBUG(*n->g(attr::Subgraph));
    }
  }

  // Merge fusible nodes into subgraphs in prim::TensorExprGroup nodes.
  void createFusionGroups(Block* block) {
    std::vector<Node*> fusion_groups;
    auto reverse_iter = block->nodes().reverse();
    Node* prev_fusion_group = nullptr;
    for (auto it = reverse_iter.begin(); it != reverse_iter.end();) {
      Node* n = *it;
      GRAPH_DEBUG("Considering node:", *n)

      for (Block* b : n->blocks()) {
        createFusionGroups(b);
      }

      if (!canHandle(n)) {
        it++;
        continue;
      }
      // There are some nodes that we can support, but we don't want to start a
      // fusion group from - skip them.
      if (n->kind() == prim::ListConstruct || n->kind() == aten::slice ||
          n->kind() == aten::unsqueeze || n->kind() == prim::ConstantChunk ||
          n->kind() == prim::Constant) {
        it++;
        continue;
      }

      Node* fusion_group = createFusionGroup(n);
      debugDumpFusionGroup("Fusion group constructed: ", fusion_group);

      // Try merging the just created fusion group into the previous one.
      // If it did not work, then put the previous fusion group into
      // fusion_groups vector - we will not touch it anymore in this loop.
      // If merging suceeded, save the merged group as the "previous" fusion
      // group so that we can try to merge the next one into it.
      if (prev_fusion_group) {
        debugDumpFusionGroup(
            "Trying to merge into the previous fusion group: ",
            prev_fusion_group);
        if (canMerge(prev_fusion_group, fusion_group)) {
          prev_fusion_group = tryMerge(prev_fusion_group, fusion_group);
          debugDumpFusionGroup(
              "Successfully merged into the previous fusion group: ",
              prev_fusion_group);
        } else {
          GRAPH_DEBUG("Cannot merge into the previous fusion group");
          fusion_groups.push_back(prev_fusion_group);
          prev_fusion_group = fusion_group;
        }
      } else {
        prev_fusion_group = fusion_group;
      }
      it = prev_fusion_group->reverseIterator();
      it++;
    }

    // We were adding groups into the vector lagging by one - catch up with
    // adding the last one
    if (prev_fusion_group) {
      fusion_groups.push_back(prev_fusion_group);
    }

    for (Node* n : fusion_groups) {
      inlineIfTooSmall(n);
    }
  }

  size_t blockSize(Block* block) {
    size_t num = 0;
    for (Node* n : block->nodes()) {
      // Don't count prim::Constants and prim::ListConstructs as these are nodes
      // we only pull in along with another, "main", node. E.g. the
      // ListConstruct nodes would also be pulled into a fusion group if they
      // are inputs of an aten::cat node.
      if (n->kind() == prim::Constant || n->kind() == prim::ListConstruct) {
        continue;
      }
      for (Block* b : n->blocks()) {
        num += blockSize(b);
      }
      num++;
    }
    return num;
  }

  bool inlineIfTooSmall(Node* n) {
    if (n->kind() != prim::TensorExprGroup) {
      return false;
    }
    auto subgraph = SubgraphUtils::getSubgraph(n);
    size_t num_modes = blockSize(subgraph->block());
    if (num_modes < min_group_size_) {
      GRAPH_UPDATE("Fusion group is too small, unmerging: ", *n);
      SubgraphUtils::unmergeSubgraph(n);
      return true;
    }
    return false;
  }

  Node* tryMerge(Node* fusion_group, Node* to_merge) {
    if (!canMerge(fusion_group, to_merge)) {
      return fusion_group;
    }

    std::vector<Node*> nodes_to_merge = {to_merge};

    if (to_merge->kind() == aten::cat) {
      Node* listconstruct = to_merge->input(0)->node();
      nodes_to_merge.push_back(listconstruct);
    }

    // First, try to move all the nodes we want to fuse next to the fusion
    // group.
    Node* move_point = fusion_group;
    for (auto n : nodes_to_merge) {
      GRAPH_UPDATE("Trying to move node next to fusion group: ", getHeader(n));
      if (!aliasDb_->moveBeforeTopologicallyValid(n, move_point)) {
        GRAPH_UPDATE("Failed to move because of AliasDB checks!");
        return fusion_group;
      }
      move_point = n;
    }

    // Now all the nodes that we're going to fuse are moved next to the fusion
    // group, so we can safely merge them into the fusion group subgraph.
    fusion_group = getOrCreateTensorExprSubgraph(fusion_group);

    for (auto n : nodes_to_merge) {
      GRAPH_UPDATE("Merging ", getHeader(n));
      mergeNodeIntoSubgraphAndUpdateAliasing(n, fusion_group);
    }
    return fusion_group;
  }

  bool allShapesAreKnown(Node* node) {
    // TODO: Relax the checks to support dynamic shapes
    for (Value* input : node->inputs()) {
      if (input->type()->cast<TensorType>()) {
        if (!input->isCompleteTensor()) {
          return false;
        }
        if (*input->type()->cast<TensorType>()->dim() == 0) {
          return false;
        }
      }
    }
    return true;
  }

  bool canFuseOnDevice(Value* v) {
    auto type = v->type()->cast<TensorType>();
    if (!type) {
      return true;
    }
    auto device = type->device();
    if (!device) {
      return false;
    }
    if (device->is_cpu()) {
      return canFuseOnCPU();
    } else if (device->is_cuda()) {
      return canFuseOnGPU();
    }
    throw std::runtime_error("Unknown device");
  }

  bool isFusableOnDevice(Node* node) {
    for (const auto& input : node->inputs()) {
      if (!canFuseOnDevice(input)) {
        return false;
      }
    }
    return true;
  }

#define REQ(cond)                           \
  if (!(cond)) {                            \
    GRAPH_DEBUG("Failed cond " #cond "\n"); \
    return false;                           \
  }

  bool canHandle(Node* node) {
    REQ(node->kind() != prim::Constant);
    REQ(allShapesAreKnown(node));
    REQ(isFusableOnDevice(node));

    // Don't include nodes whose inputs are tensor constants - we cannot handle
    // them at the moment.
    // TODO: actually support tensor constants and remove this.
    for (Value* input : node->inputs()) {
      if (input->node()->kind() == prim::Constant) {
        REQ(!input->type()->cast<TensorType>())
      }
      if (auto const& tt = input->type()->cast<TensorType>()) {
        auto st = tt->scalarType();
        if (!st) {
          // All tensor types should be known.
          return false;
        }
        if (c10::isComplexType(*st) || c10::isQIntType(*st) ||
            *st == c10::ScalarType::BFloat16) {
          return false;
        }
      }
    }
    if (node->kind() == aten::cat) {
      REQ(node->input(0)->node()->kind() == prim::ListConstruct);
      REQ(node->input(0)->uses().size() == 1);
      REQ(node->input(1)->node()->kind() == prim::Constant);
      auto const& listconstruct = node->input(0)->node();
      REQ(tensorexpr::pickDeviceType(listconstruct->inputs()));
    } else {
      REQ(tensorexpr::pickDeviceType(node->inputs()));
    }

    REQ(tensorexpr::isSupported(node));
    return true;
  }

  bool canMerge(Node* consumer, Node* producer) {
    // Only fuse within a block
    REQ(consumer->owningBlock() == producer->owningBlock());

    // Symbolic checks
    REQ(canHandle(producer) || producer->kind() == prim::TensorExprGroup);
    TORCH_INTERNAL_ASSERT(
        consumer->kind() == prim::TensorExprGroup || canHandle(consumer));

    // Device checks
    if (consumer->kind() != aten::cat && producer->kind() != aten::cat) {
      // aten::cat needs a special handling because it takes a Tensor[] as its
      // input We deal with that in the code below.
      auto consumer_device = tensorexpr::pickDeviceType(consumer->inputs());
      REQ(consumer_device);
      auto producer_device = tensorexpr::pickDeviceType(producer->inputs());
      REQ(producer_device);
      REQ(*consumer_device == *producer_device);
    }

    // Alias checks
    REQ(aliasDb_->couldMoveBeforeTopologically(producer, consumer));

    // Ops that return aliases can only be folded if this is the only use.
    if (producer->kind() == aten::slice ||
        producer->kind() == aten::unsqueeze ||
        producer->kind() == prim::ConstantChunk) {
      for (auto& use : producer->output(0)->uses()) {
        REQ(use.user == consumer);
      }
    }

    if (!consumer->hasAttribute(attr::Subgraph) &&
        consumer->kind() != prim::TensorExprGroup) {
      // Don't initiate a fusion group from prim::ListConstruct
      REQ(consumer->kind() != prim::ListConstruct);
      REQ(consumer->kind() != aten::slice);
      REQ(consumer->kind() != aten::unsqueeze);
      REQ(consumer->kind() != prim::ConstantChunk);

      // Don't initiate a fusion group just for a constant operand
      REQ(producer->kind() != prim::Constant);
    }

    if (producer->kind() == aten::cat) {
      REQ(producer->input(0)->node()->kind() == prim::ListConstruct);
      REQ(producer->input(0)->uses().size() == 1);
      REQ(producer->input(1)->node()->kind() == prim::Constant);
      auto const& listConstruct = producer->input(0)->node();
      // We're merging listconstruct->cat->consumer. cat is the producer here
      // and we cannot determine its device type - we should use device of the
      // listconstruct instead
      auto listconstruct_device =
          tensorexpr::pickDeviceType(listConstruct->inputs());
      auto consumer_device = tensorexpr::pickDeviceType(consumer->inputs());
      REQ(listconstruct_device);
      REQ(consumer_device);
      REQ(*listconstruct_device == *consumer_device);
      for (auto const& input : listConstruct->inputs()) {
        REQ(isFusableOnDevice(input->node()));
      }
    } else if (consumer->kind() == aten::cat) {
      REQ(consumer->input(0)->node()->kind() == prim::ListConstruct);
      REQ(consumer->input(0)->uses().size() == 1);
      REQ(consumer->input(1)->node()->kind() == prim::Constant);
      auto const& listConstruct = consumer->input(0)->node();
      // We're merging listconstruct->cat. cat is the consumer and listconstruct
      // is the producer. cat doesn't have its device type and thus the only
      // thing we should check is that listconstruct's device is well defined
      // (e.g. all its inputs has the same device).
      auto listconstruct_device =
          tensorexpr::pickDeviceType(listConstruct->inputs());
      REQ(listconstruct_device);
    } else {
      REQ(isFusableOnDevice(producer));
    }

    return true;
  }
#undef REQ

  void guardFusionGroup(Node* fusion_group) {
    GRAPH_DEBUG("Inserting a typecheck guard for a node", *fusion_group);
    auto subgraph = SubgraphUtils::getSubgraph(fusion_group);

    // Fixup types of the subgraph inputs
    std::vector<Value*> inputs_to_check;
    for (Value* input : fusion_group->inputs()) {
      // We only check inputs of the fusion group and expect NNC to infer
      // intermediates and outputs shapes
      if (!input->type()->cast<TensorType>()) {
        continue;
      }

      // fusion outputs are already guarded
      if (input->node()->kind() == prim::Constant ||
          input->node()->kind() == prim::FusionGroup) {
        continue;
      }
      inputs_to_check.push_back(input);
    }
    if (!inputs_to_check.size()) {
      return;
    }

    // Add prim::TypeCheck node
    //
    // TypeCheck nodes  look like the following:
    //   %out1 : Float(2, 3), %out2 : Int(10, 30), %types_match : bool =
    //   prim::TypeCheck(%inp1 : Tensor, %inp2 : Tensor)
    //
    // They have N inputs whose types we are going to check and N+1 outputs. The
    // first N outputs specify expected types and N+1-th output holds the result
    // of the check (bool).
    Node* typecheck_node =
        fusion_group->owningGraph()
            ->create(
                prim::TypeCheck, inputs_to_check, inputs_to_check.size() + 1)
            ->insertBefore(fusion_group);
    Value* typecheck_result = typecheck_node->output(inputs_to_check.size());

    std::unordered_map<Value*, Value*> typechecked_inputs;
    for (size_t i = 0; i < typecheck_node->inputs().size(); ++i) {
      typechecked_inputs[typecheck_node->input(i)] = typecheck_node->output(i);
    }

    // Fixup types of the typecheck node outputs, which are used by the op in
    // execution
    typecheck_node->output(inputs_to_check.size())->setType(BoolType::get());
    for (size_t i = 0; i < typecheck_node->inputs().size(); ++i) {
      typecheck_node->output(i)->setType(typecheck_node->input(i)->type());
    }

    // Insert if
    auto versioning_if =
        fusion_group->owningGraph()
            ->create(
                prim::If, {typecheck_result}, fusion_group->outputs().size())
            ->insertAfter(typecheck_node);
    for (size_t idx = 0; idx < fusion_group->outputs().size(); ++idx) {
      versioning_if->output(idx)->setType(fusion_group->output(idx)->type());
      fusion_group->output(idx)->replaceAllUsesWith(versioning_if->output(idx));
    }
    auto true_block = versioning_if->addBlock();
    auto false_block = versioning_if->addBlock();

    // Fill in the false block. It should contain the unoptimized
    // copy of the fused subgraph.
    WithInsertPoint guard(false_block->return_node());
    const auto subgraph_outputs = insertGraph(
        *fusion_group->owningGraph(), *subgraph, fusion_group->inputs());
    for (Value* output : subgraph_outputs) {
      false_block->registerOutput(output);
    }

    // types get copied to the fallback graph, so remove specializations before
    // replacing
    removeTensorTypeSpecializations(false_block);
    replaceBlockWithFallbackGraph(false_block, fusion_group->inputs());

    // Fill in the true block. It has all inputs type-checked and its
    // body should be the fusion group node.
    fusion_group->moveBefore(true_block->return_node());
    for (size_t idx = 0; idx < fusion_group->inputs().size(); ++idx) {
      if (typechecked_inputs.count(fusion_group->input(idx))) {
        fusion_group->replaceInput(
            idx, typechecked_inputs.at(fusion_group->input(idx)));
      }
    }
    for (Value* output : fusion_group->outputs()) {
      true_block->registerOutput(output);
    }
  }

  void guardFusionGroups(Block* block) {
    std::vector<Node*> fusion_groups;
    for (Node* n : block->nodes()) {
      for (Block* b : n->blocks()) {
        guardFusionGroups(b);
      }
      if (n->kind() == prim::TensorExprGroup) {
        fusion_groups.push_back(n);
      }
    }
    for (Node* fusion_group : fusion_groups) {
      guardFusionGroup(fusion_group);
    }
  }

  std::shared_ptr<Graph> graph_;
  std::unique_ptr<AliasDb> aliasDb_ = nullptr;

  // Minimal size of a fusion group
  size_t min_group_size_;
};

void FuseTensorExprs(std::shared_ptr<Graph>& graph, size_t min_group_size) {
  GRAPH_DUMP("Before TExprFuser: ", graph);

  // Temporary change for Block code generation.
  if (tensorexpr::getTEGenerateBlockCode()) {
    min_group_size = 1;
  }

  // Get rid of dead code so that we don't waste effort fusing it.
  EliminateDeadCode(graph);

  TensorExprFuser fuser(graph, min_group_size);
  fuser.run();

  EliminateCommonSubexpression(graph);
  EliminateDeadCode(graph);

  GRAPH_DUMP("After TExprFuser: ", graph);
}

Operation createTensorExprOp(const Node* node) {
  auto kernel =
      std::make_shared<tensorexpr::TensorExprKernel>(node->g(attr::Subgraph));
  return [kernel](Stack* stack) {
    RECORD_FUNCTION("TensorExpr", std::vector<c10::IValue>());
    if (!tensorexpr::fallbackAllowed()) {
      kernel->run(*stack);
      return 0;
    }

    try {
      kernel->run(*stack);
    } catch (const std::runtime_error& e) {
      kernel->fallback(*stack);
    }
    return 0;
  };
}

RegisterOperators TensorExprOps({
    torch::jit::Operator(
        prim::TensorExprGroup,
        createTensorExprOp,
        AliasAnalysisKind::INTERNAL_SPECIAL_CASE),
});

} // namespace jit
} // namespace torch
