/**
 * @file inference_main.cpp
 * @brief This file is an interface between the Minecraft mod and the ONNX model from 
 *        PyTorch. It works by leveraging the NVIDIA TensorRT runtime to optimize and
 *        run the ONNX model. Instead of including "jni.h" for the Java Native Interface,
 *        this file simply defines functions with the correct prototype so atomic datatypes
 *        in function arguments and returns are usable from Java. 
 */

#include <random>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <chrono>

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <stdint.h>

#define _USE_MATH_DEFINES
#include <math.h>

#include <NvOnnxParser.h>
#include <NvInfer.h>
#include <NvOnnxParser.h>
#include <cuda_runtime_api.h>

#if defined(_MSC_VER)
    #define DLL_EXPORT __declspec(dllexport)
#elif defined(__GNUC__)
    #define DLL_EXPORT __attribute__((visibility("default")))
#endif

/* This macro is used to print CUDA errors at a specific line number and return 
 * a failed operation error code */
#define CUDA_CHECK(expression) { \
        cudaError_t err = (expression);\
        if (err != cudaSuccess) { \
            printf("CUDA error at line %d. (%s)\n", __LINE__, cudaGetErrorString(err)); \
            return INFER_ERROR_FAILED_OPERATION; \
        } \
    }

/*
 * Constants:
 */
const int INFER_ERROR_INVALID_ARG             = 1;
const int INFER_ERROR_FAILED_OPERATION        = 2;
const int INFER_ERROR_INVALID_OPERATION       = 3;
const int INFER_ERROR_DESERIALIZE_CUDA_ENGINE = 4;
const int INFER_ERROR_BUILDING_FROM_ONNX      = 5;
const int INFER_ERROR_ENGINE_SAVE             = 6;
const int INFER_ERROR_SET_TENSOR_ADDRESS      = 7;
const int INFER_ERROR_ENQUEUE                 = 8;
const int INFER_ERROR_CREATE_RUNTIME          = 9;

const int BLOCK_ID_COUNT = 96;
const int EMBEDDING_DIMENSIONS = 3;
const int CHUNK_WIDTH = 16;

const int n_U = 5;    /* Number of inpainting steps per timestep */
const int n_T = 1000; /* Number of timesteps */
const int instances = n_U * n_T;

const int size_x              = 3 * 16 * 16 * 16 * sizeof(float);
const int size_x_context      = 3 * 16 * 16 * 16 * sizeof(float);
const int size_x_mask         = 1 * 16 * 16 * 16 * sizeof(float);
const int size_normal_epsilon = 3 * 16 * 16 * 16 * sizeof(float);
const int size_normal_z       = 3 * 16 * 16 * 16 * sizeof(float);
const int size_alpha     = n_T * sizeof(float);
const int size_alpha_bar = n_T * sizeof(float);
const int size_beta      = n_T * sizeof(float);

const char *onnx_file_path = "C:/Users/tbarnes/Desktop/projects/voxelnet/experiments/TestTensorRT/ddim_single_update.onnx";
const char *engine_cache_path = "C:/Users/tbarnes/Desktop/projects/voxelnet/experiments/TestTensorRT/ddim_single_update.trt";

const float block_id_embeddings[BLOCK_ID_COUNT][EMBEDDING_DIMENSIONS] = {
    { 0.0, 0.0, 0.0   }, { -2.0, -1.0, 0.1 }, { 2.0, -1.0, 0.2  }, { 0.0, -1.0, -0.1 }, 
    { -2.0, 2.0, -1.0 }, { -2.0, -1.0, -0.2}, { 0.0, -1.0, -0.3 }, { -2.0, -1.0, 0.4 }, 
    { 2.0, 2.0, 2.0   }, { 2.0, -1.0, 0.5  }, { -2.0, 2.0, 0.0  }, { 2.0, 0.0, -0.5  },  
    { 0.0, -1.0, -0.6 }, { -1.5, 1.0, 0.6  }, { 2.0, 0.0, 0.7   }, { -2.0, -1.0, -0.7}, 
    { 0.0, -1.0, 0.8  }, { 0.0, -1.0, -0.8 }, { 0.0, -1.0, -0.9 }, { 0.0, -1.0, 0.9  }, 
    { 0.0, -1.0, -1.0 }, { 0.0, -1.0, 1.0  }, { 0.0, -1.0, 0.0  }, { -2.0, 0.0, 0.1  },  
    { 2.0, 0.0, -1.1  }, { -2.0, -1.0, -1.2}, { 0.0, -1.0, 1.1  }, { 0.0, -1.0, -1.3 }, 
    { 0.0, -1.0, 1.2  }, { 0.0, -1.0, -1.4 }, { -2.0, 1.0, -1.5 }, { 0.5, 0.0, 0.5   }, 
    { 0.5, 1.0, 0.5   }, { 0.5, 0.0, 1.5   }, { 0.5, 1.0, 1.5   }, { 0.0, 0.5, 1.5   }, 
    { 0.0, 0.5, 0.5   }, { 1.0, 0.5, 1.5   }, { 1.0, 0.5, 0.5   }, { -3.0, 1.0, -2.0 }, 
    { -2.0, 1.0, 1.7  }, { 1.5, 1.0, -0.5  }, { 1.5, 2.0, -0.5  }, { 1.5, 1.0, -1.5  }, 
    { 1.5, 2.0, -1.5  }, { 2.0, 1.5, -0.5  }, { 2.0, 1.5, -1.5  }, { 1.0, 1.5, -0.5  },  
    { 1.0, 1.5, -1.5  }, { 0.0, -2.0, 1.0  }, { 0.0, -1.0, 1.1  }, { 0.0, -1.0, -1.1 }, 
    { 2.0, 0.0, -1.2  }, { 0.0, -1.0, 1.2  }, { 0.0, -1.0, -1.3 }, { 0.0, -1.0, 1.3  },
    { 0.0, -1.0, -1.4 }, { 0.0, -1.0, 1.4  }, { 0.0, -1.0, -1.5 }, { 2.0, 0.0, 1.2   }, 
    { 2.0, 0.0, -1.6  }, { 2.0, 0.0, 1.3   }, { 2.0, 0.0, -1.7  }, { 2.0, 0.0, 1.4   },
    { 2.0, 0.0, -1.8  }, { 2.0, 0.0, 1.5   }, { 2.0, 0.0, -1.9  }, { 2.0, 0.0, 1.6   }, 
    { 2.0, 0.0, -2.0  }, { 2.0, 0.0, 1.7   }, { 2.0, 0.0, -2.1  }, { 0.0, -1.0, -2.2 },   
    { 0.0, -1.0, 1.8  }, { 0.0, -1.0, -2.3 }, { 0.0, -1.0, 1.9  }, { 0.0, -1.0, -2.4 }, 
    { 0.0, -1.0, 2.0  }, { 0.0, -1.0, -2.5 }, { 0.0, -1.0, 2.1  }, { 0.0, -1.0, -2.6 }, 
    { 0.0, -1.0, 2.2  }, { 0.0, -1.0, -2.7 }, { 0.0, -1.0, 2.3  }, { 0.0, -1.0, -2.8 },   
    { 0.0, -1.0, 2.4  }, { 0.0, -1.0, -2.9 }, { 0.0, -1.0, 2.5  }, { 0.0, -1.0, -3.0 },
    { 0.0, -1.0, 2.6  }, { 0.0, -1.0, -3.1 }, { 0.0, -1.0, 2.7  }, { 0.0, -1.0, -3.2 }, 
    { 0.0, -1.0, 2.8  }, { 0.0, -1.0, -3.3 }, { 0.0, -1.0, 2.9  }, { 2.0, 0.0, -3.4  },  
};


/* 
 * Program wide global variables and buffers:
 */
static nvinfer1::IExecutionContext* context;

static std::mutex mtx;
static std::condition_variable cv;
static bool denoise_should_start;
static std::thread global_denoise_thread;

static std::atomic<bool> init_called;
static std::atomic<bool> init_complete;
static std::atomic<bool> diffusion_running;
static std::atomic<int32_t> global_timestep = 0;
static std::atomic<int32_t> global_last_error;

static float x_t       [EMBEDDING_DIMENSIONS][CHUNK_WIDTH][CHUNK_WIDTH][CHUNK_WIDTH];
static float x_t_cached[EMBEDDING_DIMENSIONS][CHUNK_WIDTH][CHUNK_WIDTH][CHUNK_WIDTH];
static float x_context [EMBEDDING_DIMENSIONS][CHUNK_WIDTH][CHUNK_WIDTH][CHUNK_WIDTH];
static float x_mask                          [CHUNK_WIDTH][CHUNK_WIDTH][CHUNK_WIDTH];

/* Middle 14^3 blocks without surrounding context */
static int cached_block_ids[CHUNK_WIDTH-2][CHUNK_WIDTH-2][CHUNK_WIDTH-2]; 

static float alpha[n_T];
static float beta[n_T];
static float alpha_bar[n_T];A


/**
 * @brief This is the main thread that's kicked off at the beginning for init.
 *        It handles the denoising process and contains all the CUDA and TensorRT code.
 *        No resources are cleaned up in this thread since it survives for the lifetime 
 *        of the program.
 *
 * @return 0 on success, error code on failure.
 */
int denoise_thread_main() {

    /*
     * Read the CUDA version 
     */
    int cuda_version;
    cudaRuntimeGetVersion(&cuda_version);
    printf("TensorRT version: %d\n", getInferLibVersion());
    printf("CUDA runtime version: %d\n", cuda_version);

    /* 
     * The full process for runtime is exporting is:
     *  Pytorch (torch.onnx.export()) --> ONNX (nvonnxparser) --> .TRT
     *
     * The code below first checks if we already have a TensorRT .trt file. 
     * If so, we use it. If not, we create the file by generating it from the ONNX file.
     *
     * Generating the .trt file from ONNX can take a while since TensorRT goes through a
     * long optimization process.
     */
    class Logger : public nvinfer1::ILogger { /* Logger class required by createInferRuntime()*/
        void log(Severity severity, const char* msg) noexcept override {
            if (severity != Severity::kINFO)
                printf("%s\n", msg);
        }
    } runtime_logger;

    FILE* file = fopen(engine_cache_path, "rb");

    nvinfer1::ICudaEngine* engine = nullptr;
    nvinfer1::IRuntime* runtime = nvinfer1::createInferRuntime(runtime_logger);

    if (!runtime) {
        printf("Failed to create TensorRT runtime\n");
        return INFER_ERROR_CREATE_RUNTIME;
    }

    if (file) {
        fseek(file, 0, SEEK_END);
        size_t engine_size = ftell(file);
        fseek(file, 0, SEEK_SET);

        std::vector<char> engine_data(engine_size);

        fread(engine_data.data(), 1, engine_size, file);
        fclose(file);

        engine = runtime->deserializeCudaEngine(engine_data.data(), engine_size);

        if (!engine) {
            printf("Failed to deserialize CUDA engine from %s\n", engine_cache_path);
            return INFER_ERROR_DESERIALIZE_CUDA_ENGINE;
        }
        printf("Loaded prebuilt TensorRT engine from %s\n", engine_cache_path);

    } else {
        /* 
         * The TensorRT .trt file wasn't found, so we need to generate it from the ONNX
         * file and cache the result for next time.
         */
        nvinfer1::IBuilder *builder = nvinfer1::createInferBuilder(runtime_logger);
        if (!builder) {
            printf("Failed to create TensorRT builder\n");
            return INFER_ERROR_BUILDING_FROM_ONNX;
        }

        nvinfer1::INetworkDefinition *network = builder->createNetworkV2(0);
        if (!network) {
            printf("Failed to create TensorRT network\n");
            return INFER_ERROR_BUILDING_FROM_ONNX;
        }

        nvinfer1::IBuilderConfig *config = builder->createBuilderConfig();
        if (!config) {
            printf("Failed to create builder config\n");
            return INFER_ERROR_BUILDING_FROM_ONNX;
        }

        nvonnxparser::IParser *parser = nvonnxparser::createParser(*network, runtime_logger);
        if (!parser) {
            printf("Failed to create ONNX parser\n");
            return INFER_ERROR_BUILDING_FROM_ONNX;
        }

        if (!parser->parseFromFile(onnx_file_path, (int)nvinfer1::ILogger::Severity::kINFO)) {
            printf("Error parsing ONNX file: %s\n", onnx_file_path);
            return INFER_ERROR_BUILDING_FROM_ONNX;
        }
        printf("Successfully parsed ONNX model\n");

        if (builder->platformHasFastFp16()) {
            config->setFlag(nvinfer1::BuilderFlag::kFP16);
            printf("Enabled FP16 precision\n");
        }

        config->setMemoryPoolLimit(nvinfer1::MemoryPoolType::kWORKSPACE, 1ULL << 30);

        nvinfer1::IHostMemory *plan = builder->buildSerializedNetwork(*network, *config);
        if (!plan) {
            printf("Failed to build serialized network\n");
            return INFER_ERROR_BUILDING_FROM_ONNX;
        }

        FILE* engine_out = fopen(engine_cache_path, "wb");

        if (!engine_out) {
            fclose(engine_out);
            printf("Failed to save engine to %s\n", engine_cache_path);
            return INFER_ERROR_ENGINE_SAVE;
        }

        fwrite(plan->data(), 1, plan->size(), engine_out);
        fclose(engine_out);
        printf("Saved serialized engine to %s\n", engine_cache_path);
 
        engine = runtime->deserializeCudaEngine(plan->data(), plan->size());
        if (!engine) {
            printf("Failed to deserialize CUDA engine\n");
            return INFER_ERROR_BUILDING_FROM_ONNX;
        }

        delete parser;
        delete config;
        delete network;
        delete builder;
    }

    /* 
     * Now that we have a TensorRT runtime, we need to setup the CUDA buffers to allow
     * the denoising model to run.
     */
    context = engine->createExecutionContext();
    if (!context) {
        printf("Failed to create execution context\n");
        return INFER_ERROR_FAILED_OPERATION;
    }

    printf("Number of layers in engine: %d\n", engine->getNbLayers());

    printf("Finished trt init\n");

    /* 
     * Compute the denoising schedule for every timestep.
     * This is equivalent to the Python code:
     *
     *  beta = torch.linspace(beta1**0.5, beta2**0.5, self.n_T) ** 2
     *  alpha = 1 - beta
     *  alpha_bar = torch.cumprod(alpha, dim=0)
     */
    {
        float beta1 = 1e-4f;
        float beta2 = 0.02f;

        float start = sqrtf(beta1);
        float end = sqrtf(beta2);
        
        float step_size = (end - start) / (n_T - 1);

        for (int i = 0; i < n_T; i++) {

            float result = start + step_size*i;
            beta[i] = result * result;

            alpha[i] = (1.0f - beta[i]);

            // Alpha bar is the cumulative product.
            alpha_bar[i] = alpha[i];

            if (i > 0) {
                alpha_bar[i] =  alpha_bar[i] * alpha_bar[i-1];
            }
        }
    }

    /* 
     * Allocate buffers for the inputs and outputs of the CUDA model
     * Some of these buffers are relatively large, such as the x_t buffer,
     * while others only contain a single floating point number.
     *
     * The tensor addresses must match the names on the Pytorch torch.onnx.export().
     */
    void* cuda_t;
    void* cuda_x_t;
    void* cuda_x_out;
    void* cuda_x_context;
    void* cuda_x_mask;
    void* cuda_alpha_t;
    void* cuda_alpha_bar_t;
    void* cuda_beta_t;

    CUDA_CHECK(cudaMalloc(&cuda_t,           sizeof(int32_t)));
    CUDA_CHECK(cudaMalloc(&cuda_x_t,         size_x)); // Input for each model step
    CUDA_CHECK(cudaMalloc(&cuda_x_out,       size_x)); // Output produced by the model
    CUDA_CHECK(cudaMalloc(&cuda_x_context,   size_x_context));
    CUDA_CHECK(cudaMalloc(&cuda_x_mask,      size_x_mask));
    CUDA_CHECK(cudaMalloc(&cuda_alpha_t,     sizeof(float)));
    CUDA_CHECK(cudaMalloc(&cuda_alpha_bar_t, sizeof(float)));
    CUDA_CHECK(cudaMalloc(&cuda_beta_t,      sizeof(float)));

    if (!context->setTensorAddress("t", cuda_t))                     { return INFER_ERROR_SET_TENSOR_ADDRESS; }
    if (!context->setTensorAddress("x_t", cuda_x_t))                 { return INFER_ERROR_SET_TENSOR_ADDRESS; }
    if (!context->setTensorAddress("x_out", cuda_x_out))             { return INFER_ERROR_SET_TENSOR_ADDRESS; }
    if (!context->setTensorAddress("context", cuda_x_context))       { return INFER_ERROR_SET_TENSOR_ADDRESS; }
    if (!context->setTensorAddress("mask", cuda_x_mask))             { return INFER_ERROR_SET_TENSOR_ADDRESS; }
    if (!context->setTensorAddress("alpha_t", cuda_alpha_t))         { return INFER_ERROR_SET_TENSOR_ADDRESS; }
    if (!context->setTensorAddress("alpha_bar_t", cuda_alpha_bar_t)) { return INFER_ERROR_SET_TENSOR_ADDRESS; }
    if (!context->setTensorAddress("beta_t", cuda_beta_t))           { return INFER_ERROR_SET_TENSOR_ADDRESS; }

    init_complete = true;

    cudaStream_t stream;
    CUDA_CHECK(cudaStreamCreate(&stream));
   
    /* 
     * This is the main loop. Each loop iteration represents one fully denoised chunk.
     * the start of the loop is blocked waiting on a start signal from startDiffusion()
     */
    for (;;) {

        /* Wait until the mutex unlocks */
        {
            std::unique_lock<std::mutex> lock(mtx);

            while (!denoise_should_start) {
                cv.wait(lock);
            }

            denoise_should_start = false; // Auto reset so it blocks next loop iteration.
        }

        /* Fill in the middle 14^3 voxels of the mask*/
        for         (int x = 1; x < CHUNK_WIDTH - 1; x++) {
            for     (int y = 1; y < CHUNK_WIDTH - 1; y++) {
                for (int z = 1; z < CHUNK_WIDTH - 1; z++) {
                    x_mask[x][y][z] = 1.0f;
                }
            }
        }

        /* Copy the "context" and "mask" tensors to the GPU */
        CUDA_CHECK(cudaMemcpy(cuda_x_context, x_context, size_x_context, cudaMemcpyHostToDevice));
        CUDA_CHECK(cudaMemcpy(cuda_x_mask, x_mask, size_x_mask, cudaMemcpyHostToDevice));

        /* Zero-out the context and mask CPU buffers so they're clean
         * for the next diffusion run. We don't need the CPU buffers anymore
         * since context and mask are already on the GPU. */
        memset(x_context, 0, sizeof(x_context));
        memset(x_mask, 0, sizeof(x_mask));
       
        /*
         * We need to fill the initial x_t with normally distributed random values.
         */
        {
            std::random_device rd;  // Seed generator
            std::mt19937 gen(rd()); // Mersenne Twister engine
            std::normal_distribution<float> dist(0.0f, 1.0f);

            for            (int w = 0; w < EMBEDDING_DIMENSIONS; w++) {
                for        (int x = 0; x < CHUNK_WIDTH; x++) {
                   for     (int y = 0; y < CHUNK_WIDTH; y++) {
                       for (int z = 0; z < CHUNK_WIDTH; z++) {
                           x_t[w][x][y][z] = dist(gen);
                       }
                   }
                }
            }
        }


        /* 
         * These 'for' loops iterate over the denoising steps. The 't' steps represent the 
         * primary denoising steps whiel the 'u' steps are used to blend the known and
         * unknown regions during in-painting. 
         */
        for (int t = n_T - 1; t >= 0; t -= 1) {
            for (int u = 0; u < n_U; u++) {

                int load_index = t * n_U + u;

                /* Copy the relevant input buffers for the TensorRT model */
                CUDA_CHECK(cudaMemcpy(cuda_t, &t, sizeof(int32_t), cudaMemcpyHostToDevice));
                CUDA_CHECK(cudaMemcpy(cuda_x_t, x_t, size_x, cudaMemcpyHostToDevice));
                CUDA_CHECK(cudaMemcpy(cuda_alpha_t, &alpha[t], sizeof(float), cudaMemcpyHostToDevice));
                CUDA_CHECK(cudaMemcpy(cuda_alpha_bar_t, &alpha_bar[t], sizeof(float), cudaMemcpyHostToDevice));
                CUDA_CHECK(cudaMemcpy(cuda_beta_t, &beta[t], sizeof(float), cudaMemcpyHostToDevice));

                /* Run the model asynchronously */
                bool enqueue_succeeded = context->enqueueV3(stream);

                if (!enqueue_succeeded) {
                    printf("enqueueV3 failed\n");
                    return INFER_ERROR_ENQUEUE;
                }

                /* Block waiting for the model to complete running */
                CUDA_CHECK(cudaStreamSynchronize(stream));

                cudaError_t result;

                {
                    std::lock_guard<std::mutex> lock(mtx);
                    result = cudaMemcpy(x_t, cuda_x_out, size_x, cudaMemcpyDeviceToHost);
                }

                CUDA_CHECK(result);
            }

            global_timestep = t;
            /* TODO: I should copy out the x_t only once it's completed all n_U iterations.
             * Otherwise, I'll be copying out a partially in-painted sample */
        }

        diffusion_running = false;
    }

    return 0; /* Never reached */
}

/** 
 * @brief This small function allows us to use the return of the denoise_thread_main
 * as the error code. This is a work-around because the C++ threading API doesn't 
 * have a way (as far as I know) to get the return value of a thread.
 */
static void denoise_thread_wrapper() {
    global_last_error = denoise_thread_main();
}

/** 
 * @brief Initialize the interface.
 * @return 0 on success
 */
extern "C" DLL_EXPORT
int32_t Java_tbarnes_diffusionmod_Inference_init(void* unused1, void* unused2) {

    if (init_called) {
        global_last_error = INFER_ERROR_INVALID_OPERATION;
        return INFER_ERROR_INVALID_OPERATION;
    }

    denoise_thread = std::thread(denoise_thread_wrapper);

    if (!denoise_thread.joinable()) {

        printf("Thread creation failed\n");
        global_last_error = INFER_ERROR_INVALID_OPERATION;
        return INFER_ERROR_FAILED_OPERATION;
    }

    init_called = true;
    return 0;
}

/** 
 * @brief setContextBlock 
 *  Set the context for denoising to allow the in-painting process to generate 
 *  a new chunk that matches neighbor chunks.
 * @param: x 
 * @param: y 
 * @param: z 
 * @param: block_id 
 * @return: 0 on success
 */
extern "C" DLL_EXPORT
int32_t Java_tbarnes_diffusionmod_Inference_setContextBlock(void* unused1, void* unused2,
        int32_t x, int32_t y, int32_t z, int32_t block_id) {

    if (x < 0 || x >= CHUNK_WIDTH ||
        y < 0 || y >= CHUNK_WIDTH ||
        z < 0 || z >= CHUNK_WIDTH ||
        block_id < 0 || block_id >= BLOCK_ID_COUNT) {

        global_last_error = INFER_ERROR_INVALID_ARG;
        return INFER_ERROR_INVALID_ARG;
    }

    /* Use the embedding matrix to find the vector for this block_id. */

    for (int dim = 0; dim < EMBEDDING_DIMENSIONS; dim++) {
        x_context[dim][x][y][z] = block_id_embeddings[block_id][dim];
    }
    
    x_mask[x][y][z] = 1.0f;

    return 0;
}

/** 
 * @brief startDiffusion 
 */
extern "C" DLL_EXPORT
int32_t Java_tbarnes_diffusionmod_Inference_startDiffusion(void* unused1, void* unused2) {
    
    if (diffusion_running) {
        global_last_error = INFER_ERROR_INVALID_OPERATION;
        return INFER_ERROR_INVALID_OPERATION;
    }

    global_timestep = n_T;
    diffusion_running = true;

    {
        std::lock_guard<std::mutex> lock(mtx);
        denoise_should_start = true;
        cv.notify_one();
    }

    return 0;
}

/** 
 * @brief 
 * @return Integer for cached timestep in range [0, 1000)
 * Timestep 0 is the fully denoised time.
 */
extern "C" DLL_EXPORT
int32_t Java_tbarnes_diffusionmod_Inference_getCurrentTimestep(void* unused1, void* unused2) { 
    return global_timestep;
}

/** 
 * @brief cacheCurrentTimestepForReading 
 * @return Integer for cached timestep in range [0, 1000)
 * Timestep 0 is the fully denoised time.
 */
extern "C" DLL_EXPORT
int32_t Java_tbarnes_diffusionmod_Inference_cacheCurrentTimestepForReading(void* unused1, void* unused2) { 

    {
        std::lock_guard<std::mutex> lock(mtx);
        memcpy(x_t_cached, x_t, sizeof(x_t));
    }

    /* Perform matrix multiply of x_t and transpose(block_id_embeddings)
     * Since we only care about the index of the smallest element in each row of the output
     * 4096 x BLOCK_ID_COUNT matrix, we don't need to actually store the entire matrix. */

    for (int x = 1; x < 15; x++) {
        for (int y = 1; y < 15; y++) {
            for (int z = 1; z < 15; z++) {

                float min_distance = FLT_MAX;
                int closest_id = 0;

                for (int i = 0; i < BLOCK_ID_COUNT; i++) {
                    float distance = 0.0f;

                    for (int j = 0; j < EMBEDDING_DIMENSIONS; j++) {
                        float diff = x_t_cached[j][x][y][z] - block_id_embeddings[i][j];
                        distance += diff * diff;
                    }

                    if (distance < min_distance) {
                        min_distance = distance;
                        closest_id = i;
                    }
                }

                cached_block_ids[x-1][y-1][z-1] = closest_id;
            }
        }
    }

    return global_timestep;
}

/** 
 * @brief readBlockFromCachedtimestep
 * Retrieve a block_id from the cached chunk at an (x, y, z) position.
 * Integer inputs must be in range [0, 14)
 * @param: x 
 * @param: y 
 * @param: z 
 * @return: block_id of cached block.
 */
extern "C" DLL_EXPORT
int32_t Java_tbarnes_diffusionmod_Inference_readBlockFromCachedTimestep(void* unused1, void* unused2, 
        int32_t x, int32_t y, int32_t z) {

    return cached_block_ids[x][y][z];
}


extern "C" DLL_EXPORT
int32_t Java_tbarnes_diffusionmod_Inference_getLastError(void* unused1, void* unused2) {

    return (int32_t)error_return;
}

#if 1
void main() {

    int result = Java_tbarnes_diffusionmod_Inference_init(0, 0);
    
    result = Java_tbarnes_diffusionmod_Inference_startDiffusion(0, 0);

    printf("End of main");

    int32_t last_step = 1000;

    while (1) {

        int32_t step = Java_tbarnes_diffusionmod_Inference_getCurrentTimestep(NULL, NULL);

        if (step < last_step) {
            last_step = step;

            float sum = 0.0f;

            for (int x = 0; x < 14; x++) {
                for (int y = 0; y < 14; y++) {
                    for (int z = 0; z < 14; z++) {
                        sum += (float) Java_tbarnes_diffusionmod_Inference_readBlockFromCachedTimestep(NULL, NULL, x, y, z);

                    }
                }
            }
            
            printf("step = %d, sum = %f\n", step, sum);
            fflush(stdout);
        }
    }
}
#endif

