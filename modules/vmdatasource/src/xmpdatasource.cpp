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
#include "xmpdatasource.hpp"

#include "umf/metadatastream.hpp"
#include "umf/format_const.hpp"

#include "xmpschemasource.hpp"
#include "xmpmetadatasource.hpp"
#include "xmpstatsource.hpp"

#include <XMPUtils.hpp>

#define UMF_GLOBAL_NEXT_ID "next-id"
#define UMF_GLOBAL_CHECKSUM "media-checksum"
#define UMF_GLOBAL_HINT "hint-encryption"

#define UMF_INTERNAL "internal"
#define UMF_VIDEO_SEGMENTS "video-segments"
#define UMF_VIDEO_SEGMENT_NAME "name"
#define UMF_VIDEO_SEGMENT_FPS "fps"
#define UMF_VIDEO_SEGMENT_TIME "time"
#define UMF_VIDEO_SEGMENT_DURATION "duration"
#define UMF_VIDEO_SEGMENT_RESOLUTION_W "resolution_width"
#define UMF_VIDEO_SEGMENT_RESOLUTION_H "resolution_height"


using namespace std;
using namespace umf;

void XMPDataSource::initialize()
{
    try
    {
        if(!SXMPMeta::Initialize())
        {
            UMF_EXCEPTION(DataStorageException, "Internal error during "
                "initialization of XMPMeta.");
        }
        SXMPMeta::RegisterNamespace(UMF_NS, "umf", NULL);
        XMP_OptionBits xmpFilesOpts = static_cast<XMP_OptionBits>(0);
#ifdef __linux__
        xmpFilesOpts |= kXMPFiles_ServerMode;
#endif
        if(!SXMPFiles::Initialize(xmpFilesOpts))
        {
            UMF_EXCEPTION(DataStorageException, "Internal error during "
                "initialization of XMPFiles.");
        }
    }
    catch (const XMP_Error& e)
    {
        UMF_EXCEPTION(DataStorageException, e.GetErrMsg());
    }
    catch (const std::exception& e)
    {
        UMF_EXCEPTION(DataStorageException, e.what());
    }
}



void XMPDataSource::terminate()
{
    try
    {
        SXMPFiles::Terminate();
        SXMPMeta::Terminate();
    }
    catch (...)
    {
        // do nothing
    }
}


XMPDataSource::XMPDataSource()
  : IDataSource(), xmp(nullptr), metadataSource(nullptr), compressor(nullptr), encryptor(nullptr)
{
    schemaCompression = make_shared<umf::MetadataSchema>(COMPRESSED_DATA_SCHEMA_NAME);
    UMF_METADATA_BEGIN(COMPRESSED_DATA_DESC_NAME);
        UMF_FIELD_STR(COMPRESSION_ALGO_PROP_NAME);
        UMF_FIELD_STR(COMPRESSED_DATA_PROP_NAME);
    UMF_METADATA_END(schemaCompression);

    schemaEncryption = make_shared<umf::MetadataSchema>(ENCRYPTED_DATA_SCHEMA_NAME);
    UMF_METADATA_BEGIN(ENCRYPTED_DATA_DESC_NAME);
        UMF_FIELD_STR(ENCRYPTION_HINT_PROP_NAME);
        UMF_FIELD_STR(ENCRYPTED_DATA_PROP_NAME);
    UMF_METADATA_END(schemaEncryption);
}

XMPDataSource::~XMPDataSource()
{

}

void XMPDataSource::setCompressor(const umf_string &id)
{
    if(!id.empty())
    {
        compressor = Compressor::create(id);
    }
    else
    {
        compressor = nullptr;
    }
}


void XMPDataSource::setEncryptor(std::shared_ptr<Encryptor> _encryptor)
{
    encryptor = _encryptor;
}


void XMPDataSource::loadXMPstructs()
{
    std::shared_ptr<SXMPMeta> tmpXMP = make_shared<SXMPMeta>();
    xmpFile.GetXMP(tmpXMP.get());

    std::shared_ptr<XMPMetadataSource> tmpMetaSource;
    std::shared_ptr<XMPSchemaSource> tmpSchemaSource;
    std::shared_ptr<XMPStatSource> tmpStatSource;
    tmpSchemaSource = make_shared<XMPSchemaSource>(tmpXMP);
    tmpMetaSource = make_shared<XMPMetadataSource>(tmpXMP);
    tmpStatSource = make_shared<XMPStatSource>(tmpXMP);
    if(!tmpMetaSource || !tmpSchemaSource || !tmpStatSource)
    {
        UMF_EXCEPTION(DataStorageException, "Failed to create metadata source or schema source");
    }

    //load standard UMF metadata and decompress them if there is a corresponding schema
    //or pass them further
    try
    {
        //load standard UMF metadata and decrypt them if there is a corresponding schema
        //or pass them further
        std::map<umf_string, std::shared_ptr<MetadataSchema> > eSchemas;
        tmpSchemaSource->load(eSchemas);
        auto itEncryption = eSchemas.find(ENCRYPTED_DATA_SCHEMA_NAME);
        if(itEncryption != eSchemas.end())
        {
            MetadataStream eStream;
            eStream.addSchema(itEncryption->second);
            tmpMetaSource->loadSchema(ENCRYPTED_DATA_SCHEMA_NAME, eStream);
            MetadataSet eSet = eStream.queryBySchema(ENCRYPTED_DATA_SCHEMA_NAME);
            std::shared_ptr<Metadata> eItem = eSet[0];
            umf_string hint      = eItem->getFieldValue(ENCRYPTION_HINT_PROP_NAME);
            umf_string encodedB64 = eItem->getFieldValue(ENCRYPTED_DATA_PROP_NAME);
            bool ignoreBad = (openMode & MetadataStream::OpenModeFlags::IgnoreUnknownEncryptor) != 0;
            if(!encryptor)
            {
                if(!ignoreBad)
                {
                    UMF_EXCEPTION(IncorrectParamException,
                                  "No decryption algorithm provided for encrypted data");
                }
            }
            else
            {
                string decodedFromB64;
                XMPUtils::DecodeFromBase64(encodedB64.data(), encodedB64.length(), &decodedFromB64);
                umf_rawbuffer encrypted(decodedFromB64.c_str(), decodedFromB64.size());
                string theData;
                try
                {
                    encryptor->decrypt(encrypted, theData);
                }
                catch(Exception& ee)
                {
                    //if we've failed with decryption (whatever the reason was)
                    //and we're allowed to ignore that
                    if(!ignoreBad)
                    {
                        string message = "Decryption failed: " + string(ee.what()) + ", hint: " + hint;
                        UMF_EXCEPTION(IncorrectParamException, message);
                    }
                }
                //replace tmp XMP entities
                tmpXMP->ParseFromBuffer(theData.c_str(), theData.size(), 0);
                tmpSchemaSource = make_shared<XMPSchemaSource>(tmpXMP);
                tmpMetaSource = make_shared<XMPMetadataSource>(tmpXMP);
                tmpStatSource = make_shared<XMPStatSource>(tmpXMP);
                if(!tmpMetaSource || !tmpSchemaSource || !tmpStatSource)
                {
                    UMF_EXCEPTION(DataStorageException,
                                  "Failed to create metadata source, schema source or stat source");
                }
            }
        }

        //load standard UMF metadata and decompress them if there is a corresponding schema
        //or pass them further
        std::map<umf_string, std::shared_ptr<MetadataSchema> > cSchemas;
        tmpSchemaSource->load(cSchemas);
        auto itCompression = cSchemas.find(COMPRESSED_DATA_SCHEMA_NAME);
        if(itCompression != cSchemas.end())
        {
            MetadataStream cStream;
            cStream.addSchema(itCompression->second);
            tmpMetaSource->loadSchema(COMPRESSED_DATA_SCHEMA_NAME, cStream);
            MetadataSet cSet = cStream.queryBySchema(COMPRESSED_DATA_SCHEMA_NAME);
            std::shared_ptr<Metadata> cItem = cSet[0];
            umf_string algo    = cItem->getFieldValue(COMPRESSION_ALGO_PROP_NAME);
            umf_string encoded = cItem->getFieldValue(COMPRESSED_DATA_PROP_NAME);
            bool ignoreBad = (openMode & MetadataStream::OpenModeFlags::IgnoreUnknownCompressor) != 0;
            try
            {
                std::shared_ptr<Compressor> decompressor = Compressor::create(algo);
                string decoded;
                XMPUtils::DecodeFromBase64(encoded.data(), encoded.length(), &decoded);
                umf_rawbuffer compressed(decoded.c_str(), decoded.size());
                string theData;
                decompressor->decompress(compressed, theData);
                //replace tmp XMP entities
                tmpXMP->ParseFromBuffer(theData.c_str(), theData.size(), 0);
                tmpSchemaSource = make_shared<XMPSchemaSource>(tmpXMP);
                tmpMetaSource = make_shared<XMPMetadataSource>(tmpXMP);
                tmpStatSource = make_shared<XMPStatSource>(tmpXMP);
                if(!tmpMetaSource || !tmpSchemaSource || !tmpStatSource)
                {
                    UMF_EXCEPTION(DataStorageException,
                                  "Failed to create metadata source, schema source or stat source");
                }
            }
            catch(IncorrectParamException& ce)
            {
                //if there's no such compressor and we're allowed to ignore that
                if(!ignoreBad)
                {
                    UMF_EXCEPTION(IncorrectParamException, ce.what());
                }
            }
            catch(InternalErrorException& ce)
            {
                //if there was an error during decompression
                if(!ignoreBad)
                {
                    UMF_EXCEPTION(IncorrectParamException, ce.what());
                }
            }
        }
    }
    catch(const XMP_Error& e)
    {
        UMF_EXCEPTION(DataStorageException, e.GetErrMsg());
    }
    catch(const std::exception& e)
    {
        UMF_EXCEPTION(DataStorageException, e.what());
    }

    xmp = tmpXMP;
    schemaSource = tmpSchemaSource;
    metadataSource = tmpMetaSource;
    statSource = tmpStatSource;
}


void XMPDataSource::saveXMPstructs()
{
    std::shared_ptr<SXMPMeta> tmpXMP = xmp;

    //Sometimes compressed&encoded data is bigger than the source data
    //but there's no need to compare their sizes and write the smallest one.
    //Because due to RDF's verbosity it happens only when the source data is small.
    //That's why the economy wouldn't be significant.
    if(compressor)
    {
        string buffer;
        XMP_OptionBits options = kXMP_ReadOnlyPacket | kXMP_UseCompactFormat;
        tmpXMP->SerializeToBuffer(&buffer, options, 0, NULL);
        umf_rawbuffer compressed;
        compressor->compress(buffer, compressed);
        string encoded;
        XMPUtils::EncodeToBase64 (compressed.data(), compressed.size(), &encoded);

        //save compressed data as UMF metadata with corresponding schema
        MetadataStream cStream;
        cStream.addSchema(schemaCompression);
        shared_ptr<Metadata> cMetadata;
        cMetadata = make_shared<Metadata>(schemaCompression->findMetadataDesc(COMPRESSED_DATA_DESC_NAME));
        cMetadata->push_back(FieldValue(COMPRESSION_ALGO_PROP_NAME, compressor->getId()));
        cMetadata->push_back(FieldValue(COMPRESSED_DATA_PROP_NAME,  encoded));
        cStream.add(cMetadata);

        tmpXMP = make_shared<SXMPMeta>();
        std::shared_ptr<XMPMetadataSource> cMetaSource;
        std::shared_ptr<XMPSchemaSource> cSchemaSource;
        cSchemaSource = make_shared<XMPSchemaSource>(tmpXMP);
        cMetaSource = make_shared<XMPMetadataSource>(tmpXMP);
        if (!cMetaSource || !cSchemaSource)
        {
            UMF_EXCEPTION(DataStorageException, "Failed to create compressed metadata source or schema source");
        }

        try
        {
            cMetaSource->saveSchema(schemaCompression, cStream.getAll());
            cSchemaSource->save(schemaCompression);
        }
        catch(const XMP_Error& e)
        {
            UMF_EXCEPTION(DataStorageException, e.GetErrMsg());
        }
        catch(const std::exception& e)
        {
            UMF_EXCEPTION(DataStorageException, e.what());
        }
        IdType cNextId = 1;
        tmpXMP->SetProperty_Int64(UMF_NS, UMF_GLOBAL_NEXT_ID, cNextId);
    }

    //existing encryptor should mean that we need an encryption
    if(encryptor)
    {
        string buffer;
        XMP_OptionBits options = kXMP_ReadOnlyPacket | kXMP_UseCompactFormat;
        tmpXMP->SerializeToBuffer(&buffer, options, 0, NULL);
        umf_rawbuffer encrypted;
        encryptor->encrypt(buffer, encrypted);
        string encoded;
        XMPUtils::EncodeToBase64(encrypted.data(), encrypted.size(), &encoded);

        //save encrypted data as UMF metadata with corresponding schema
        MetadataStream eStream;
        eStream.addSchema(schemaEncryption);
        shared_ptr<Metadata> eMetadata;
        eMetadata = make_shared<Metadata>(schemaEncryption->findMetadataDesc(ENCRYPTED_DATA_DESC_NAME));
        eMetadata->push_back(FieldValue(ENCRYPTION_HINT_PROP_NAME, encryptor->getHint()));
        eMetadata->push_back(FieldValue(ENCRYPTED_DATA_PROP_NAME, encoded));
        eStream.add(eMetadata);

        tmpXMP = make_shared<SXMPMeta>();
        std::shared_ptr<XMPMetadataSource> eMetaSource;
        std::shared_ptr<XMPSchemaSource> eSchemaSource;
        eSchemaSource = make_shared<XMPSchemaSource>(tmpXMP);
        eMetaSource = make_shared<XMPMetadataSource>(tmpXMP);
        if(!eMetaSource || !eSchemaSource)
        {
            UMF_EXCEPTION(DataStorageException, "Failed to create compressed metadata source or schema source");
        }

        try
        {
            eMetaSource->saveSchema(schemaEncryption, eStream.getAll());
            eSchemaSource->save(schemaEncryption);
        }
        catch(const XMP_Error& e)
        {
            UMF_EXCEPTION(DataStorageException, e.GetErrMsg());
        }
        catch(const std::exception& e)
        {
            UMF_EXCEPTION(DataStorageException, e.what());
        }
        IdType eNextId = 1;
        tmpXMP->SetProperty_Int64(UMF_NS, UMF_GLOBAL_NEXT_ID, eNextId);
    }

    if(xmpFile.CanPutXMP(*tmpXMP))
    {
        xmpFile.PutXMP(*tmpXMP);
    }
    else
    {
        UMF_EXCEPTION(InternalErrorException, "Can't write XMP data to file");
    }
}


void XMPDataSource::openFile(const umf_string& fileName, MetadataStream::OpenMode mode)
{
    try
    {
        xmp = make_shared<SXMPMeta>();
        openMode = mode;
        metaFileName = fileName;
        XMP_OptionBits modeFlags;
        if (mode & MetadataStream::Update)
        {
            modeFlags = kXMPFiles_OpenForUpdate;
        }
        else
        {
            modeFlags = kXMPFiles_OpenForRead;
        }
        XMP_OptionBits opts = modeFlags | kXMPFiles_OpenUseSmartHandler;
        bool opened = xmpFile.OpenFile(metaFileName, kXMP_UnknownFile, opts);
        if (!opened)
        {
            opts = modeFlags | kXMPFiles_OpenUsePacketScanning;
            opened = xmpFile.OpenFile(metaFileName, kXMP_UnknownFile, opts);
            if (!opened)
            {
                UMF_EXCEPTION(DataStorageException, "Could not open XMP file.");
            }
        }

        loadXMPstructs();
   }
    catch (const XMP_Error& e)
    {
        UMF_EXCEPTION(DataStorageException, e.GetErrMsg());
    }
    catch (const std::exception& e)
    {
        UMF_EXCEPTION(DataStorageException, e.what());
    }
}



void XMPDataSource::closeFile()
{
    try
    {
        metadataSource.reset();
        schemaSource.reset();
        statSource.reset();
        xmp.reset();
        xmpFile.CloseFile();
    }
    catch(const XMP_Error& e)
    {
        UMF_EXCEPTION(DataStorageException, e.GetErrMsg());
    }
    catch (const std::exception& e)
    {
        UMF_EXCEPTION(DataStorageException, e.what());
    }
}


void XMPDataSource::loadSchema(const umf_string& schemaName, MetadataStream& stream)
{
    metadataSourceCheck();
    try
    {
        metadataSource->loadSchema(schemaName, stream);
    }
    catch(const XMP_Error& e)
    {
        UMF_EXCEPTION(DataStorageException, e.GetErrMsg());
    }
    catch(const std::exception& e)
    {
        UMF_EXCEPTION(DataStorageException, e.what());
    }
}



void XMPDataSource::loadProperty(const umf_string &schemaName, const umf_string &propertyName, MetadataStream &stream)
{
    metadataSourceCheck();
    try
    {
        metadataSource->loadProperty(schemaName, propertyName, stream);
    }
    catch(const XMP_Error& e)
    {
        UMF_EXCEPTION(DataStorageException, e.GetErrMsg());
    }
    catch(const std::exception& e)
    {
        UMF_EXCEPTION(DataStorageException, e.what());
    }
}


void XMPDataSource::saveSchema(const std::shared_ptr<MetadataSchema>& schemaDesc, const MetadataSet& mdSet)
{
    metadataSourceCheck();
    try
    {
        metadataSource->saveSchema(schemaDesc, mdSet);
    }
    catch(const XMP_Error& e)
    {
        UMF_EXCEPTION(DataStorageException, e.GetErrMsg());
    }
    catch(const std::exception& e)
    {
        UMF_EXCEPTION(DataStorageException, e.what());
    }
}



void XMPDataSource::save(const std::shared_ptr<umf::MetadataSchema>& schema)
{
    if (schema == nullptr)
    {
        UMF_EXCEPTION(NullPointerException, "Couldn't save nullptr schema");
    }
    schemaSourceCheck();
    try
    {
        schemaSource->save(schema);
    }
    catch(const XMP_Error& e)
    {
        UMF_EXCEPTION(DataStorageException, e.GetErrMsg());
    }
    catch(const std::exception& e)
    {
        UMF_EXCEPTION(DataStorageException, e.what());
    }
}


void XMPDataSource::serializeAndParse()
{
    string tempBuffer;
    XMP_OptionBits options = kXMP_ReadOnlyPacket | kXMP_UseCompactFormat;
    xmp->SerializeToBuffer(&tempBuffer, options, 0, NULL);

    xmp = make_shared<SXMPMeta>();
    xmp->ParseFromBuffer(tempBuffer.c_str(), tempBuffer.size(), 0);
    schemaSource = make_shared<XMPSchemaSource>(xmp);
    metadataSource = make_shared<XMPMetadataSource>(xmp);
    statSource = make_shared<XMPStatSource>(xmp);
}


void XMPDataSource::remove(const vector<IdType>& ids)
{
    metadataSourceCheck();
    try
    {
        metadataSource->remove(ids);

        /* TODO:
         * Existing implementation requires metadata re-reading here,
         * so a quick workaround is made below:
         * instead of saving to file we just put everything to buffer and get back.
         * To be fixed to eliminate re-reading in long term future.
         */
        serializeAndParse();
    }
    catch(const XMP_Error& e)
    {
        UMF_EXCEPTION(DataStorageException, e.GetErrMsg());
    }
    catch(const std::exception& e)
    {
        UMF_EXCEPTION(DataStorageException, e.what());
    }
}

void XMPDataSource::load(std::map<umf_string, std::shared_ptr<MetadataSchema> >& schemas)
{
    schemaSourceCheck();
    try
    {
        schemaSource->load(schemas);
    }
    catch(const XMP_Error& e)
    {
        UMF_EXCEPTION(DataStorageException, e.GetErrMsg());
    }
    catch(const std::exception& e)
    {
        UMF_EXCEPTION(DataStorageException, e.what());
    }
}

void XMPDataSource::saveStats(const std::vector< std::shared_ptr<Stat> >& stats)
{
    statSourceCheck();
    try
    {
        statSource->save(stats);
    }
    catch(const XMP_Error& e)
    {
        UMF_EXCEPTION(DataStorageException, e.GetErrMsg());
    }
    catch(const std::exception& e)
    {
        UMF_EXCEPTION(DataStorageException, e.what());
    }
}

void XMPDataSource::loadStats(std::vector< std::shared_ptr<Stat> >& stats)
{
    statSourceCheck();
    try
    {
        statSource->load(stats);
    }
    catch(const XMP_Error& e)
    {
        UMF_EXCEPTION(DataStorageException, e.GetErrMsg());
    }
    catch(const std::exception& e)
    {
        UMF_EXCEPTION(DataStorageException, e.what());
    }
}

void XMPDataSource::clear()
{
    metadataSourceCheck();
    schemaSourceCheck();
    statSourceCheck();
    try
    {
        metadataSource->clear();
        schemaSource->clear();
        statSource->clear();
    }
    catch(const XMP_Error& e)
    {
        UMF_EXCEPTION(DataStorageException, e.GetErrMsg());
    }
    catch(const std::exception& e)
    {
        UMF_EXCEPTION(DataStorageException, e.what());
    }

}

IdType XMPDataSource::loadId()
{
    XMP_Int64 id;
    if(!xmp->GetProperty_Int64(UMF_NS, UMF_GLOBAL_NEXT_ID, &id, nullptr))
    {
        return 0;
    }
    return id;
}

void XMPDataSource::save(const IdType &id)
{
    xmp->SetProperty_Int64(UMF_NS, UMF_GLOBAL_NEXT_ID, id);
}


void XMPDataSource::pushChanges()
{
    saveXMPstructs();

    closeFile();
    openFile(this->metaFileName, this->openMode);
}

void XMPDataSource::metadataSourceCheck()
{
    if (!metadataSource)
    {
        UMF_EXCEPTION(DataStorageException, "Metadata source doesn't exist");
    }
}

void XMPDataSource::schemaSourceCheck()
{
    if (!schemaSource)
    {
        UMF_EXCEPTION(DataStorageException, "Schema source doesn't exist");
    }
}

void XMPDataSource::statSourceCheck()
{
    if (!statSource)
    {
        UMF_EXCEPTION(DataStorageException, "Statistics source doesn't exist");
    }
}

void XMPDataSource::removeSchema(const umf_string &schemaName)
{
    /* TODO:
     * Existing implementation requires metadata re-reading here,
     * so a quick workaround is made below:
     * instead of saving to file we just put everything to buffer and get back.
     * To be fixed to eliminate re-reading in long term future.
     */
    schemaSource->remove(schemaName);
    serializeAndParse();
}

std::string XMPDataSource::computeChecksum(long long& XMPPacketSize, long long& XMPPacketOffset)
{
    try
    {
        loadXMPstructs();

        umf_string checksum;
        xmpFile.ComputeChecksum(&checksum, &XMPPacketSize, &XMPPacketOffset);
        return checksum;
    }
    catch(const XMP_Error& e)
    {
        UMF_EXCEPTION(DataStorageException, e.GetErrMsg());
    }
    catch(const std::exception& e)
    {
        UMF_EXCEPTION(DataStorageException, e.what());
    }
    catch(...)
    {
        UMF_EXCEPTION(DataStorageException, "Internal failure. Checksum can't be computed!");
    }
}


std::string XMPDataSource::loadChecksum()
{
    std::string checksum;
    if(!xmp->GetProperty(UMF_NS, UMF_GLOBAL_CHECKSUM, &checksum, NULL) )
        return "";
    return checksum;
}

void XMPDataSource::saveChecksum(const umf_string& checksum)
{
    xmp->SetProperty(UMF_NS, UMF_GLOBAL_CHECKSUM, checksum.c_str());
}


std::string XMPDataSource::loadHintEncryption()
{
    std::string hint;
    if(!xmp->GetProperty(UMF_NS, UMF_GLOBAL_HINT, &hint, NULL))
        return "";
    return hint;
}


void XMPDataSource::saveHintEncryption(const umf_string& hint)
{
    xmp->SetProperty(UMF_NS, UMF_GLOBAL_HINT, hint.c_str());
}


void XMPDataSource::saveVideoSegments(const std::vector<std::shared_ptr<MetadataStream::VideoSegment>>& segments)
{
    xmp->DeleteProperty(UMF_NS, UMF_INTERNAL);
    if (!segments.empty())
    {
        umf_string pathToInternalData;
        xmp->AppendArrayItem(UMF_NS, UMF_INTERNAL, kXMP_PropValueIsArray, NULL, kXMP_PropValueIsStruct);
        SXMPUtils::ComposeArrayItemPath(UMF_NS, UMF_INTERNAL, kXMP_ArrayLastItem, &pathToInternalData);
        xmp->SetStructField(UMF_NS, pathToInternalData.c_str(), UMF_NS, UMF_VIDEO_SEGMENTS, nullptr, kXMP_PropValueIsArray);

        std::for_each(segments.begin(), segments.end(), [&](const std::shared_ptr<MetadataStream::VideoSegment>& segment)
        {
            umf_string pathToSegmentsArray;
            SXMPUtils::ComposeStructFieldPath(UMF_NS, pathToInternalData.c_str(), UMF_NS, UMF_VIDEO_SEGMENTS, &pathToSegmentsArray);
            xmp->AppendArrayItem(UMF_NS, pathToSegmentsArray.c_str(), kXMP_PropValueIsArray, nullptr, kXMP_PropValueIsStruct);
            umf_string pathToSegment;
            SXMPUtils::ComposeArrayItemPath(UMF_NS, pathToSegmentsArray.c_str(), kXMP_ArrayLastItem, &pathToSegment);

            umf_string tmpPath;

            if (segment->getTitle().empty() || segment->getFPS() <= 0 || segment->getTime() < 0)
                UMF_EXCEPTION(DataStorageException, "Invalid video segment: title, fps or timestamp value(s) is/are invalid!");

            SXMPUtils::ComposeStructFieldPath(UMF_NS, pathToSegment.c_str(), UMF_NS, UMF_VIDEO_SEGMENT_NAME, &tmpPath);
            xmp->SetProperty(UMF_NS, tmpPath.c_str(), segment->getTitle().c_str());

            SXMPUtils::ComposeStructFieldPath(UMF_NS, pathToSegment.c_str(), UMF_NS, UMF_VIDEO_SEGMENT_FPS, &tmpPath);
            xmp->SetProperty_Float(UMF_NS, tmpPath.c_str(), segment->getFPS());

            SXMPUtils::ComposeStructFieldPath(UMF_NS, pathToSegment.c_str(), UMF_NS, UMF_VIDEO_SEGMENT_TIME, &tmpPath);
            xmp->SetProperty_Int64(UMF_NS, tmpPath.c_str(), segment->getTime());

            if (segment->getDuration() > 0)
            {
                SXMPUtils::ComposeStructFieldPath(UMF_NS, pathToSegment.c_str(), UMF_NS, UMF_VIDEO_SEGMENT_DURATION, &tmpPath);
                xmp->SetProperty_Int64(UMF_NS, tmpPath.c_str(), segment->getDuration());
            }

            long width, height;
            segment->getResolution(width, height);
            if (width > 0 && height > 0)
            {
                SXMPUtils::ComposeStructFieldPath(UMF_NS, pathToSegment.c_str(), UMF_NS, UMF_VIDEO_SEGMENT_RESOLUTION_W, &tmpPath);
                xmp->SetProperty_Int64(UMF_NS, tmpPath.c_str(), width);

                SXMPUtils::ComposeStructFieldPath(UMF_NS, pathToSegment.c_str(), UMF_NS, UMF_VIDEO_SEGMENT_RESOLUTION_H, &tmpPath);
                xmp->SetProperty_Int64(UMF_NS, tmpPath.c_str(), height);
            }
        });
    }
}

void XMPDataSource::loadVideoSegments(std::vector<std::shared_ptr<MetadataStream::VideoSegment>>& segments)
{
    umf_string pathToInternalData;
    SXMPUtils::ComposeArrayItemPath(UMF_NS, UMF_INTERNAL, kXMP_ArrayLastItem, &pathToInternalData);

    umf_string pathToSegmentsArray;
    SXMPUtils::ComposeStructFieldPath(UMF_NS, pathToInternalData.c_str(), UMF_NS, UMF_VIDEO_SEGMENTS, &pathToSegmentsArray);

    SXMPIterator segmentsIter(*xmp, UMF_NS, pathToSegmentsArray.c_str(), kXMP_IterJustChildren);
    umf_string pathToSegment;
    while (segmentsIter.Next(nullptr, &pathToSegment))
    {
	umf_string tmpPath, segmentTitle;
	double fps;
	long long timestamp, duration;
	XMP_Int32 width, height;

	SXMPUtils::ComposeStructFieldPath(UMF_NS, pathToSegment.c_str(), UMF_NS, UMF_VIDEO_SEGMENT_NAME, &tmpPath);
	if(!xmp->GetProperty(UMF_NS, tmpPath.c_str(), &segmentTitle, 0) )
	    UMF_EXCEPTION(DataStorageException, "Broken video segment's title");

	SXMPUtils::ComposeStructFieldPath(UMF_NS, pathToSegment.c_str(), UMF_NS, UMF_VIDEO_SEGMENT_FPS, &tmpPath);
	if(!xmp->GetProperty_Float(UMF_NS, tmpPath.c_str(), &fps, 0) )
	    UMF_EXCEPTION(DataStorageException, "Broken video segment's FPS");

	SXMPUtils::ComposeStructFieldPath(UMF_NS, pathToSegment.c_str(), UMF_NS, UMF_VIDEO_SEGMENT_TIME, &tmpPath);
	if(!xmp->GetProperty_Int64(UMF_NS, tmpPath.c_str(), &timestamp, 0) )
	    UMF_EXCEPTION(DataStorageException, "Broken video segment's timestamp");

	std::shared_ptr<MetadataStream::VideoSegment> segment = std::make_shared<MetadataStream::VideoSegment>(segmentTitle, fps, timestamp);

	SXMPUtils::ComposeStructFieldPath(UMF_NS, pathToSegment.c_str(), UMF_NS, UMF_VIDEO_SEGMENT_DURATION, &tmpPath);
	if(xmp->GetProperty_Int64(UMF_NS, tmpPath.c_str(), &duration, 0) )
	    segment->setDuration(duration);

	SXMPUtils::ComposeStructFieldPath(UMF_NS, pathToSegment.c_str(), UMF_NS, UMF_VIDEO_SEGMENT_RESOLUTION_W, &tmpPath);

	if(xmp->GetProperty_Int(UMF_NS, tmpPath.c_str(), &width, 0) )
	{
	    SXMPUtils::ComposeStructFieldPath(UMF_NS, pathToSegment.c_str(), UMF_NS, UMF_VIDEO_SEGMENT_RESOLUTION_H, &tmpPath);
	    if(xmp->GetProperty_Int(UMF_NS, tmpPath.c_str(), &height, 0))
		segment->setResolution((long)width, (long)height);
	}
	segments.push_back(segment);
    }
}
