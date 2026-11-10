// check_metallib: loads the libraries build_metallib.py made, the way the Metal backend does, and specializes every
// function in them (g_SpecConstants = 0), so a library that the runtime cannot use shows up before the game runs.
//
//   clang++ -std=c++20 -fobjc-arc -O2 check_metallib.mm -framework Metal -framework Foundation -o check_metallib
//   check_metallib <out_dir>/lib
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <chrono>
#include <cstdio>

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <lib_dir>\n", argv[0]);
    return 2;
  }
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
      std::fprintf(stderr, "no Metal device\n");
      return 1;
    }
    std::printf("%s, argument buffers tier %d\n", [[device name] UTF8String], int(device.argumentBuffersSupport) + 1);
    MTLFunctionConstantValues* constants = [MTLFunctionConstantValues new];
    uint32_t zero = 0;
    [constants setConstantValue:&zero type:MTLDataTypeUInt atIndex:0];
    size_t libraries = 0, functions = 0, failed = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int shard = 0; shard < 16; ++shard) {
      NSString* path = [NSString stringWithFormat:@"%s/bo2zm_%x.metallib", argv[1], shard];
      NSError* error = nil;
      id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:path] error:&error];
      if (!library) {
        std::printf("%s: %s\n", [path UTF8String], error ? [[error localizedDescription] UTF8String] : "missing");
        ++failed;
        continue;
      }
      ++libraries;
      for (NSString* name in [library functionNames]) {
        @autoreleasepool {
          id<MTLFunction> f = [library newFunctionWithName:name constantValues:constants error:&error];
          ++functions;
          if (!f) {
            ++failed;
            std::printf("%s: %s\n", [name UTF8String], error ? [[error localizedDescription] UTF8String] : "?");
          }
        }
      }
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("%zu libraries, %zu functions specialized, %zu failures, %.1f s\n", libraries, functions, failed,
                seconds);
    return failed ? 1 : 0;
  }
}
