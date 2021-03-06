/* Copyright (c) 2020 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License,
 * attached with Common Clause Condition 1.0, found in the LICENSES directory.
 */

#include "nebula/NebulaChaosPlan.h"
#include "nebula/NebulaUtils.h"
#include "core/WaitAction.h"
#include <folly/FileUtil.h>
#include <folly/json.h>

DEFINE_string(email_to, "", "mail list");

namespace chaos {
namespace nebula_chaos {

// static
std::unique_ptr<PlanContext>
NebulaChaosPlan::loadInstanceFromFile(const std::string& instanceFilename,
                                      std::vector<NebulaInstance*>& insts,
                                      std::string& email) {
    std::string jsonStr;
    if (access(instanceFilename.c_str(), F_OK) != 0) {
        LOG(ERROR) << "Instance file not exists " << instanceFilename;
        return nullptr;
    }

    if (!folly::readFile(instanceFilename.c_str(), jsonStr)) {
        LOG(ERROR) << "Parse Instance file " << instanceFilename << " failed!";
        return nullptr;
    }
    auto jsonObj = folly::parseJson(jsonStr);
    VLOG(1) << folly::toPrettyJson(jsonObj);

    auto instances = jsonObj.at("instances");
    CHECK(instances.isArray());
    email = jsonObj.getDefault("email", FLAGS_email_to).asString();
    auto ctx = std::make_unique<PlanContext>();
    // Ensure the vector large enough.
    ctx->storageds.reserve(instances.size());
    ctx->metads.reserve(instances.size());
    auto it = instances.begin();

    while (it != instances.end()) {
        CHECK(it->isObject());
        auto type = it->at("type").asString();
        auto installPath = it->at("install_dir").asString();
        auto confPath = it->at("conf_dir").asString();
        auto host = it->at("host").asString();
        auto user = it->at("user").asString();
        LOG(INFO) << "Load inst type " << type
                  << ", installPath " << installPath
                  << ", confPath " << confPath
                  << ", host " << host
                  << ", user " << user;
        if (type == "storaged") {
            NebulaInstance inst(host,
                                installPath,
                                NebulaInstance::Type::STORAGE,
                                confPath,
                                user);
            CHECK(inst.init());
            ctx->storageds.emplace_back(std::move(inst));
            insts.emplace_back(&ctx->storageds.back());
        } else if (type == "graphd") {
            NebulaInstance inst(host,
                                installPath,
                                NebulaInstance::Type::GRAPH,
                                confPath,
                                user);
            CHECK(inst.init());
            ctx->graphd = std::move(inst);
            insts.emplace_back(&ctx->graphd);
        } else if (type == "metad") {
            NebulaInstance inst(host,
                                installPath,
                                NebulaInstance::Type::META,
                                confPath,
                                user);
            CHECK(inst.init());
            ctx->metads.emplace_back(std::move(inst));
            insts.emplace_back(&ctx->metads.back());
        } else {
            LOG(FATAL) << "Unknown type " << type;
        }
        it++;
    }

    return ctx;
}


// static
std::unique_ptr<NebulaChaosPlan>
NebulaChaosPlan::loadActionFromFile(const std::string& actionFilename,
                                    std::unique_ptr<PlanContext> ctx,
                                    const std::vector<NebulaInstance*>& insts,
                                    const std::string emailTo) {
    std::string jsonStr;
    if (access(actionFilename.c_str(), F_OK) != 0) {
        LOG(ERROR) << "Action file not exists " << actionFilename;
        return nullptr;
    }

    if (!folly::readFile(actionFilename.c_str(), jsonStr)) {
        LOG(ERROR) << "Parse action file " << actionFilename << " failed!";
        return nullptr;
    }

    auto jsonObj = folly::parseJson(jsonStr);
    VLOG(1) << folly::toPrettyJson(jsonObj);
    auto planName = jsonObj.at("name").asString();
    auto concurrency = jsonObj.at("concurrency").asInt();
    auto rolling = jsonObj.getDefault("rolling_table", true).asBool();
    auto actionsItem = jsonObj.at("actions");

    auto plan = std::make_unique<NebulaChaosPlan>(std::move(ctx), concurrency, emailTo, planName);
    std::vector<std::unique_ptr<core::Action>> actions;
    LoadContext loadCtx;
    loadCtx.insts = std::move(insts);
    loadCtx.gClient = plan->getGraphClient();
    loadCtx.rolling = rolling;
    loadCtx.planCtx = plan->getContext();

    {
        auto actionIt = actionsItem.begin();
        while (actionIt != actionsItem.end()) {
            CHECK(actionIt->isObject());
            auto action = Utils::loadAction(*actionIt, loadCtx);
            CHECK(action != nullptr);
            action->setId(actions.size());
            actions.emplace_back(std::move(action));
            actionIt++;
        }
    }
    {
        auto actionIt = actionsItem.begin();
        int32_t index = 0;
        while (actionIt != actionsItem.end()) {
            CHECK(actionIt->isObject());
            LOG(INFO) << "Load the " << index << " action's dependees!";
            auto depends = actionIt->at("depends");
            for (auto& dependeeIndex : depends) {
                auto i = dependeeIndex.asInt();
                actions[index]->addDependee(actions[i].get());
            }
            actionIt++;
            index++;
        }
    }
    plan->addActions(std::move(actions));
    return plan;
}


// static
std::unique_ptr<NebulaChaosPlan>
NebulaChaosPlan::loadFromFile(const std::string& instanceFilename,
                              const std::string& actionFilename) {
    std::vector<NebulaInstance*> insts;
    std::string emailTo;
    auto ctx = loadInstanceFromFile(instanceFilename, insts, emailTo);
    if (ctx == nullptr) {
        return nullptr;
    }
    return loadActionFromFile(actionFilename, std::move(ctx), insts, emailTo);
}

}   // namespace nebula_chaos
}   // namespace chaos
