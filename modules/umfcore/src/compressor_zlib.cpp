/*
 * Copyright 2015 Intel(r) Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http ://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include "compressor_zlib.hpp"
#include <cstdint>
#include "zlib.h"

namespace umf {

static const size_t startingBlockSize = sizeof(umf_integer);

void CompressorZlib::compress(const umf_string &input, umf_rawbuffer& output)
{
    size_t srcLen = input.length()*sizeof(umf_string::value_type);
    if(srcLen)
    {
        size_t destBound = compressBound((uLong)srcLen);
        size_t destLength = destBound;

        // We should also keep the size of source data
        // for further decompression
        std::vector<std::uint8_t> destBuf(destBound + startingBlockSize);

        *((umf_integer*)destBuf.data()) = umf_integer(srcLen);
        std::uint8_t* toCompress = destBuf.data() + startingBlockSize;

        //level should be default or from 0 to 9 (regulates speed/size ratio)
        int level = Z_DEFAULT_COMPRESSION;
        int rcode = compress2(toCompress, (uLongf*)&destLength,
                              (const Bytef*)input.c_str(), (uLong)srcLen, level);
        destLength += startingBlockSize;

        if(rcode != Z_OK)
        {
            if(rcode == Z_MEM_ERROR)
            {
                UMF_EXCEPTION(InternalErrorException, "Out of memory");
            }
            else
            {
                UMF_EXCEPTION(InternalErrorException, "Compressing error occured");
                //Z_BUF_ERROR if there was not enough room in the output buffer,
                //Z_STREAM_ERROR if the level parameter is invalid.
            }
        }

        output = umf_rawbuffer((const char*)destBuf.data(), destLength);
    }
    else
    {
        //buffer containing only size of data which is 0
        output = umf_rawbuffer(startingBlockSize);
    }
}


void CompressorZlib::decompress(const umf_rawbuffer& input, umf_string& output)
{
    if(input.size())
    {
        //input data also keeps the size of source data
        //since zlib doesn't save it at compression time
        size_t  compressedSize = input.size() - startingBlockSize;
        std::uint8_t* compressedBuf = (std::uint8_t*)input.data();
        size_t decompressedSize = size_t(*((umf_integer*)compressedBuf));
        if(decompressedSize)
        {
            compressedBuf += startingBlockSize;
            size_t gotDecompressedSize = decompressedSize;
            std::vector<std::uint8_t> decompressedBuf(decompressedSize);
            int rcode = uncompress(decompressedBuf.data(), (uLongf*)&gotDecompressedSize,
                                   compressedBuf, (uLong)compressedSize);
            if(rcode != Z_OK)
            {
                if(rcode == Z_MEM_ERROR)
                {
                    UMF_EXCEPTION(InternalErrorException, "Out of memory");
                }
                else
                {
                    UMF_EXCEPTION(InternalErrorException, "Decompressing error occured");
                    //Z_BUF_ERROR if there was not enough room in the output buffer,
                    //Z_DATA_ERROR if the input data was corrupted or incomplete
                }
            }

            if(gotDecompressedSize != decompressedSize)
            {
                UMF_EXCEPTION(InternalErrorException,
                              "The size of decompressed data doesn't match to source size");
            }

            output = umf_string((const char*)decompressedBuf.data(), decompressedSize);
        }
        else
        {
            output = umf_string();
        }
    }
    else
    {
        output = umf_string();
    }
}

} /* umf */
