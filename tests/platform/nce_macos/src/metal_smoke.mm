// Host GPU allocations must still work once the guest window is reserved.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <string>

#include "report.h"
#include "nce_runtime.h"
#include "suite.h"

namespace ncev {

bool MetalSmoke(std::string & detail) {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil) {
            detail = "pas de périphérique Metal";
            return false;
        }
        constexpr NSUInteger Size = 256u << 20;
        id<MTLBuffer> buffer = [device newBufferWithLength:Size options:MTLResourceStorageModeShared];
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> cmd = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
        [blit fillBuffer:buffer range:NSMakeRange(0, Size) value:0x5A];
        [blit endEncoding];
        [cmd commit];
        [cmd waitUntilCompleted];
        const auto * bytes = static_cast<const unsigned char *>([buffer contents]);
        const std::uint64_t addr = reinterpret_cast<std::uint64_t>(bytes);
        const bool filled = bytes[0] == 0x5A && bytes[Size - 1] == 0x5A;
        const bool outside = !InGuestWindow(addr) && !InGuestWindow(addr + Size - 1);
        detail = Fmt("%s, tampon 256 Mio à %#llx (%s), remplissage GPU %s", [[device name] UTF8String],
                     (unsigned long long)addr, outside ? "hors fenêtre" : "DANS la fenêtre", filled ? "ok" : "faux");
        return filled && outside && cmd.status == MTLCommandBufferStatusCompleted;
    }
}

} // namespace ncev
