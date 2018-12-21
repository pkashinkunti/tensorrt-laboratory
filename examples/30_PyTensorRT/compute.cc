/* Copyright (c) 2018, NVIDIA CORPORATION. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of NVIDIA CORPORATION nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <future>
#include <map>
#include <memory>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "tensorrt/playground/core/thread_pool.h"
#include "tensorrt/playground/core/memory/descriptor.h"
#include "tensorrt/playground/core/memory/host_memory.h"
#include "tensorrt/playground/cuda/memory/device_memory.h"
#include "tensorrt/playground/runtime.h"
#include "tensorrt/playground/model.h"
#include "tensorrt/playground/bindings.h"
#include "tensorrt/playground/inference_manager.h"

using namespace yais;
using namespace yais::Memory;
using namespace yais::TensorRT;
/*
struct AsyncCompute
{
    AsyncCompute() {}

    AsyncCompute(AsyncCompute&&) = delete;
    AsyncCompute& operator=(AsyncCompute&&) = delete;

    AsyncCompute(const AsyncCompute&) = delete;
    AsyncCompute& operator=(const AsyncCompute&) = delete;

    virtual ~AsyncCompute() {}

    template<typename T>
    class Result
    {
      public:
        Result() {}
        Result(Result&& other) : m_Promise{std::move(other.m_Promise)} {}
        virtual ~Result() {}

        using ResultHandle = std::unique_ptr<T>;
        using Future = std::shared_future<ResultHandle>;

        void operator()(ResultHandle result)
        {
            DLOG(INFO) << "Promise SetValue";
            m_Promise.set_value(std::move(result));
        }

        Future GetFuture()
        {
            return m_Promise.get_future();
        }

      private:
        std::promise<ResultHandle> m_Promise;
    };

}

*/

struct InferModel
{
    InferModel(std::shared_ptr<Model> model, std::shared_ptr<InferenceManager> resources)
      : m_Model{model}, m_Resources{resources} {}

    InferModel(InferModel&&) = delete;
    InferModel& operator=(InferModel&&) = delete;

    InferModel(const InferModel&) = delete;
    InferModel& operator=(const InferModel&) = delete;

    virtual ~InferModel() {}

    using BindingsHandle = std::shared_ptr<Bindings>;
    using HostMap = std::map<std::string, DescriptorHandle<HostMemory>>;
    using DeviceMap = std::map<std::string, DescriptorHandle<DeviceMemory>>;

    using PreFn = std::function<void(BindingsHandle)>;
    using PostFn = std::function<void(BindingsHandle)>;

    template<typename T>
    class Result
    {
      public:
        Result() {}
        Result(Result&& other) : m_Promise{std::move(other.m_Promise)} {}
        virtual ~Result() {}

        using ResultHandle = std::unique_ptr<T>;
        using Future = std::shared_future<ResultHandle>;
        using PostFn = std::function<ResultHandle(std::shared_ptr<Bindings>)>;

        void operator()(ResultHandle result)
        {
            DLOG(INFO) << "Promise SetValue";
            m_Promise.set_value(std::move(result));
        }

        Future GetFuture()
        {
            return m_Promise.get_future();
        }

      private:
        std::promise<ResultHandle> m_Promise;
    };

    template<typename T>
    typename Result<T>::Future Compute(PreFn pre, typename Result<T>::PostFn post)
    {
        auto result = std::make_shared<Result<T>>();
        auto wrapped_post = [post, result](BindingsHandle bindings) mutable {
            DLOG(INFO) << "Wrapped Post";
            auto val = post(bindings);
            (*result)(std::move(val));
        };
        Execute(pre, wrapped_post);
        auto future = result->GetFuture();
        LOG(INFO) << "Wait on Future";
        // CHECK(future.get());
        LOG(INFO) << "Compute Finished";
        return future;
    }

  private:
    void Execute(PreFn Pre, PostFn Post)
    {
        Workers("pre").enqueue([this, Pre, Post]() mutable {
            auto bindings = InitializeBindings();
            Pre(bindings);
            Workers("cuda").enqueue([this, bindings, Post]() mutable {
                bindings->CopyToDevice(bindings->InputBindings());
                auto trt_ctx = Infer(bindings);
                bindings->CopyFromDevice(bindings->OutputBindings());
                Workers("post").enqueue([this, bindings, trt_ctx, Post]() mutable {
                    trt_ctx->Synchronize();
                    trt_ctx.reset();
                    bindings->CopyToDevice(bindings->InputBindings());
                    bindings->Synchronize();
                    Post(bindings);
                    LOG(INFO) << "ResetBindings";
                    bindings.reset();
                    LOG(INFO) << "Execute Finished";
                });
            });
        });
    }

    BindingsHandle InitializeBindings()
    {
        auto buffers = m_Resources->GetBuffers();
        return buffers->CreateBindings(m_Model);
    }

    auto Infer(BindingsHandle& bindings) -> std::shared_ptr<ExecutionContext>
    {
        auto trt_ctx = m_Resources->GetExecutionContext(bindings->GetModel());
        trt_ctx->Infer(bindings);
        return trt_ctx;
    }

    inline ThreadPool& Workers(std::string name)
    {
        return m_Resources->GetThreadPool(name);
    }

    std::shared_ptr<Model> m_Model;
    std::shared_ptr<InferenceManager> m_Resources;
};

static bool ValidateEngine(const char *flagname, const std::string &value)
{
    struct stat buffer;
    return (stat(value.c_str(), &buffer) == 0);
}

DEFINE_string(engine, "/path/to/tensorrt.engine", "TensorRT serialized engine");
DEFINE_validator(engine, &ValidateEngine);
DEFINE_int32(contexts, 1, "Number of Execution Contexts");
DEFINE_int32(buffers, 0, "Number of Buffers (default: 2x contexts)");
DEFINE_int32(prethreads, 1, "Number of preproessing threads");
DEFINE_int32(cudathreads, 1, "Number of cuda kernel launching threads");
DEFINE_int32(postthreads, 3, "Number of postprocessing threads");

int main(int argc, char *argv[])
{
    FLAGS_alsologtostderr = 1; // Log to console
    ::google::InitGoogleLogging("TensorRT Inference");
    ::google::ParseCommandLineFlags(&argc, &argv, true);

    auto contexts = FLAGS_contexts;
    auto buffers = FLAGS_buffers ? FLAGS_buffers : 2 * FLAGS_contexts;

    auto resources = std::make_shared<InferenceManager>(contexts, buffers);

    resources->SetThreadPool("pre", std::make_unique<ThreadPool>(FLAGS_prethreads));
    resources->SetThreadPool("cuda", std::make_unique<ThreadPool>(FLAGS_cudathreads));
    resources->SetThreadPool("post", std::make_unique<ThreadPool>(FLAGS_postthreads));

    auto model = Runtime::DeserializeEngine(FLAGS_engine);
    resources->RegisterModel("flowers", model);
    resources->AllocateResources();
    LOG(INFO) << "Resources Allocated";

    InferModel flowers(model, resources);

    // std::vector<std::unique_ptr<bool>> results;
    for(int i=0; i<10; i++) {
        auto result = 
            flowers.Compute<bool>(
                [](std::shared_ptr<Bindings> bindings) mutable {
                    // TODO: Copy Input Data to Host Input Bindings
                    DLOG(INFO) << "Pre";
                    bindings->SetBatchSize(bindings->GetModel()->GetMaxBatchSize());
                },
                [](std::shared_ptr<Bindings> bindings) mutable -> std::unique_ptr<bool> {
                    DLOG(INFO) << "Post";
                    return std::move(std::make_unique<bool>(true));
                }
            );
        CHECK(result.get());
    }

    return 0;
}