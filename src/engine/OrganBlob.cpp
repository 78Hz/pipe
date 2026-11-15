// Pipe - the organ's precomputed flue pipes, embedded at build time (OrganBlob.S).
#include <cstddef>
extern "C" const unsigned char pipe_organ_blob[];
extern "C" const unsigned char pipe_organ_blob_end[];
namespace pipe {
const unsigned char* organBlobData() { return pipe_organ_blob; }
size_t organBlobSize() { return (size_t)(pipe_organ_blob_end - pipe_organ_blob); }
}
