#include "dom/dom_manager.h"

#include <queue>

#include "dom/render_manager.h"
#include "dom/diff_utils.h"
#include "dom/dom_node.h"
#include "dom/dom_event.h"

namespace hippy {
inline namespace dom {

using DomNode = hippy::DomNode;

constexpr uint32_t kInvalidListenerId = 0;
constexpr char kOnDomCreated[] = "onDomCreated";
constexpr char kOnDomUpdated[] = "onDomUpdate";
constexpr char kOnDomDeleted[] = "kOnDomDeleted";

DomManager::DomManager(uint32_t root_id) : root_id_(root_id) {
  root_node_ = std::make_shared<DomNode>(root_id, -1, 0);
  dom_node_registry_.AddNode(root_node_);
}

DomManager::~DomManager() = default;

void DomManager::CreateDomNodes(std::vector<std::shared_ptr<DomNode>> &&nodes) {
  for (const auto& node : nodes) {
    std::shared_ptr<DomNode> parent_node = dom_node_registry_.GetNode(node->GetPid());
    if (parent_node == nullptr) {
      // it = nodes.erase(it);
      continue;
    }
    node->SetRenderInfo({node->GetPid(), node->GetIndex(), true});
    // 解析布局属性
    node->ParseLayoutStyleInfo();
    parent_node->AddChildAt(node, node->GetIndex());

    dom_node_registry_.AddNode(node);
    HandleEvent(std::make_shared<DomEvent>(kOnDomCreated, node, true, true));
  }

  if (!nodes.empty()) {
    batched_operations_.emplace_back([this, moved_nodes = std::move(nodes)]() mutable {
      render_manager_->CreateRenderNode(std::move(moved_nodes));
    });
  }
}

void DomManager::UpdateDomNodes(std::vector<std::shared_ptr<DomNode>> &&nodes) {
  for (auto it = nodes.begin(); it != nodes.end(); it++) {
    std::shared_ptr<DomNode> node = dom_node_registry_.GetNode((*it)->GetId());
    if (node == nullptr) {
      it = nodes.erase(it);
      continue;
    }
    // diff props
    DomValueMap style_diff = DiffUtils::DiffProps(node->GetStyle(), it->get()->GetStyle());
    DomValueMap ext_diff = DiffUtils::DiffProps(node->GetExtStyle(), it->get()->GetExtStyle());
    style_diff.insert(ext_diff.begin(), ext_diff.end());
    it->get()->SetDiffStyle(std::move(style_diff));

    HandleEvent(std::make_shared<DomEvent>(kOnDomUpdated, node, true, true));
  }

  if (!nodes.empty()) {
    batched_operations_.emplace_back([this, moved_nodes = std::move(nodes)]() mutable {
      render_manager_->UpdateRenderNode(std::move(moved_nodes));
    });
  }
}

void DomManager::DeleteDomNodes(std::vector<std::shared_ptr<DomNode>> &&nodes) {
  for (auto it = nodes.begin(); it != nodes.end(); it++) {
    std::shared_ptr<DomNode> node = dom_node_registry_.GetNode((*it)->GetId());
    if (node == nullptr) {
      it = nodes.erase(it);
      continue;
    }
    std::shared_ptr<DomNode> parent_node = node->GetParent();
    if (parent_node != nullptr) {
      parent_node->RemoveChildAt(parent_node->IndexOf(node));
    }

    dom_node_registry_.RemoveNode(node->GetId());
    HandleEvent(std::make_shared<DomEvent>(kOnDomDeleted, node, true, true));
  }

  if (!nodes.empty()) {
    batched_operations_.emplace_back([this, moved_nodes = std::move(nodes)]() mutable {
      render_manager_->DeleteRenderNode(std::move(moved_nodes));
    });
  }
}

void DomManager::BeginBatch() {}

void DomManager::EndBatch() {
  // 触发布局计算
  layout_changed_nodes_.clear();
  root_node_->DoLayout();
  const auto &udpate_node = layout_changed_nodes_;
  if (!layout_changed_nodes_.empty()) {
    batched_operations_.emplace_back(
        [this, &udpate_node]() { render_manager_->UpdateLayout(udpate_node); });
  }
  for (auto &batch_operation : batched_operations_) {
    batch_operation();
  }
  batched_operations_.clear();
  render_manager_->Batch();
}

uint32_t DomManager::AddEventListener(uint32_t id, const std::string &name, bool use_capture,
                                      const EventCallback &cb) {
  auto node = dom_node_registry_.GetNode(id);
  if (!node) {
    return kInvalidListenerId;
  }
  return node->AddEventListener(name, use_capture, cb);
}

void DomManager::CallFunction(uint32_t id, const std::string &name,
                              const DomValue &param,
                              const CallFunctionCallback &cb) {
  auto node = dom_node_registry_.GetNode(id);
  if (node == nullptr) {
    return;
  }
  node->CallFunction(name, param, cb);
}

std::tuple<float, float> DomManager::GetRootSize() {
  TDF_BASE_DCHECK(root_node_);
  return root_node_->GetLayoutSize();
}

void DomManager::SetRootSize(float width, float height) {
  TDF_BASE_CHECK(root_node_);
  root_node_->SetLayoutSize(width, height);
}

void DomManager::AddLayoutChangedNode(const std::shared_ptr<DomNode> &node) {
  layout_changed_nodes_.push_back(node);
}

void DomManager::SetRootNode(const std::shared_ptr<DomNode> &root_node) {
  if (root_node) {
    if (root_node_) {
      dom_node_registry_.RemoveNode(root_node_->GetId());
    }
    root_node_ = root_node;
    dom_node_registry_.AddNode(root_node);
  }
}

void DomManager::HandleEvent(const std::shared_ptr<DomEvent> &event) {
  auto weak_target = event->GetTarget();
  auto event_name = event->GetType();
  auto target = weak_target.lock();
  if (target) {
    std::queue<std::shared_ptr<DomNode>> capture_list = {};
    capture_list.push(target);
    if (!event->IsPreventCapture()) {
      // 获取捕获列表
      while (auto parent = target->GetParent()) {
        capture_list.push(parent);
        target = parent;
      }
      // 执行捕获流程
      while (auto capture_node = capture_list.front()) {
        capture_list.pop();
        event->SetCurrentTarget(capture_node); // 设置当前节点，cb里会用到
        auto listeners = capture_node->GetEventListener(event_name, true);
        for (const auto &listener: listeners) {
          listener->cb(event);
        }
        if (event->IsPreventCapture()) {
          // cb 内部调用了 event.StopPropagation 会阻止捕获
          break;
        }
      }
    }
    // 执行冒泡流程
    // 注：就算阻止了冒泡，本身节点cb还是要调用的，因此执行完了target的cb再判断是否阻止冒泡
    while (auto bubble_node = target) {
      event->SetCurrentTarget(bubble_node);
      auto listeners = bubble_node->GetEventListener(event_name, false);
      for (const auto &listener: listeners) {
        listener->cb(event);
      }
      if (event->IsPreventBubble()) {
        break;
      }
    }
  }
}

void DomManager::DomNodeRegistry::AddNode(const std::shared_ptr<DomNode> &node) {
  nodes_.insert(std::make_pair(node->GetId(), node));
}

std::shared_ptr<DomNode> DomManager::DomNodeRegistry::GetNode(int32_t id) {
  auto found = nodes_.find(id);
  if (found == nodes_.end()) {
    return nullptr;
  }
  return found->second;
}

void DomManager::DomNodeRegistry::RemoveNode(int32_t id) { nodes_.erase(id); }

}  // namespace dom
}  // namespace hippy
