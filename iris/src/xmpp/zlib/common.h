#ifndef ZLIB_COMMON_H
#define ZLIB_COMMON_H

#define CHUNK_SIZE 1024

static void initZStream(z_stream *z)
{
    z->next_in   = nullptr;
    z->avail_in  = 0;
    z->total_in  = 0;
    z->next_out  = nullptr;
    z->avail_out = 0;
    z->total_out = 0;
    z->msg       = nullptr;
    z->state     = nullptr;
    z->zalloc    = Z_NULL;
    z->zfree     = Z_NULL;
    z->opaque    = Z_NULL;
    z->data_type = Z_BINARY;
    z->adler     = 0;
    z->reserved  = 0;
}

#endif // ZLIB_COMMON_H
