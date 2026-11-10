// Pipe - the measured rooms' sample data, embedded at build time (IrBlob.S).
#include <cstddef>

#ifdef PIPE_HAVE_IR_BLOB
extern "C" const unsigned char pipe_ir_blob[];
extern "C" const unsigned char pipe_ir_blob_end[];
namespace pipe {
const unsigned char* irBlobData() { return pipe_ir_blob; }
size_t irBlobSize() { return (size_t)(pipe_ir_blob_end - pipe_ir_blob); }
}
#else
namespace pipe {
const unsigned char* irBlobData() { return nullptr; }
size_t irBlobSize() { return 0; }
}
#endif
