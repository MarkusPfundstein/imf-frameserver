#include "asdcp.h"
#include <string>
#include <KM_fileio.h>
#include <AS_02.h>
#include <WavFileWriter.h>
#include <cstdlib>

namespace ASDCP {
    Result_t MD_to_PCM_ADesc(ASDCP::MXF::WaveAudioDescriptor* ADescObj, ASDCP::PCM::AudioDescriptor& ADesc);
}

using namespace ASDCP;

const ui32_t FRAME_BUFFER_SIZE = 4 * Kumu::Megabyte;

Result_t read_PCM_file(asset_t *asset, asdcp_on_pcm_frame_func on_frame, void *user_data) {
    AESDecContext* Context = 0;
    HMACContext* HMAC = 0;
    AS_02::PCM::MXFReader Reader;
    PCM::FrameBuffer FrameBuffer;
    ui32_t last_sample = 0;
    // TO-DO: Figure out correct edit_rate here. probably 25
    // if i make 48000 , callback will be 1 sample (length: 6, L: 3, R: 3)
    Rational edit_rate = Rational(25, 1);
    ASDCP::MXF::WaveAudioDescriptor *wave_descriptor = 0;

    Result_t result = Reader.OpenRead(asset->mxf_path, edit_rate);

    if (!KM_SUCCESS(result)) {
        fprintf(stderr, "error opening %s\n", asset->mxf_path);
        return result;
    }

    ASDCP::MXF::InterchangeObject* tmp_obj = 0;
    result = Reader.OP1aHeader().GetMDObjectByType(DefaultCompositeDict().ul(MDD_WaveAudioDescriptor), &tmp_obj);

    if (!KM_SUCCESS(result)) {
        fprintf(stderr, "error reading OP1aHeader %s\n", asset->mxf_path);
        return result;
    }

    wave_descriptor = dynamic_cast<ASDCP::MXF::WaveAudioDescriptor*>(tmp_obj);
    if (wave_descriptor == 0) {
        fprintf(stderr, "File does not contain an essence descriptor.\n");
        return RESULT_FAIL;
    }

    if ( wave_descriptor->ContainerDuration.get() == 0 ) {
        fprintf(stderr, "ContainerDuration not set in file descriptor, attempting to use index duration.\n");
        last_sample = Reader.AS02IndexReader().GetDuration();
    } else {
        last_sample = (ui32_t)wave_descriptor->ContainerDuration;
    }

    if (last_sample == 0) {
        fprintf(stderr, "ContainerDuration not set in index, attempting to use Duration from SourceClip.\n");
        result = Reader.OP1aHeader().GetMDObjectByType(DefaultCompositeDict().ul(MDD_SourceClip), &tmp_obj);
        if (KM_SUCCESS(result)) {
            ASDCP::MXF::SourceClip *sourceClip = dynamic_cast<ASDCP::MXF::SourceClip*>(tmp_obj);
            if (!sourceClip->Duration.empty()) {
                last_sample = (ui32_t)sourceClip->Duration;
            }
        }
    }

    if (last_sample == 0) {
        fprintf(stderr, "Unable to determine file duration.\n");
        return RESULT_FAIL;
    }

    /*
    if (last_sample != asset->end_frame) {
        fprintf(stderr, "last_sample: %d != %d\n", last_sample, asset->end_frame);
        return RESULT_FAIL;
    }
    */

    FrameBuffer.Capacity(AS_02::MXF::CalcFrameBufferSize(*wave_descriptor, edit_rate));
    
    int last_frame = AS_02::MXF::CalcFramesFromDurationInSamples(asset->end_frame, *wave_descriptor, edit_rate);

    /*
    if (ASDCP_SUCCESS(result) && Options.key_flag) {
        Context = new AESDecContext;
        result = Context->InitKey(Options.key_value);

        if (ASDCP_SUCCESS(result) && Options.read_hmac ) {
            WriterInfo Info;
            Reader.FillWriterInfo(Info);

            if (Info.UsesHMAC) {
                HMAC = new HMACContext;
                result = HMAC->InitKey(Options.key_value, Info.LabelSetType);
            } else {
                fputs("File does not contain HMAC values, ignoring -m option.\n", stderr);
            }
        }
    }
    */

    for (unsigned int i = asset->start_frame; i < last_frame; i++) {
        result = Reader.ReadFrame(i, FrameBuffer, Context, HMAC);

        if (!ASDCP_SUCCESS(result)) {
            break;
        }
        if ( FrameBuffer.Size() != FrameBuffer.Capacity()) {
            FrameBuffer.Size(FrameBuffer.Capacity());
        }

        unsigned char *buf = (unsigned char*)malloc(FrameBuffer.Size());
        memcpy(buf, FrameBuffer.Data(), FrameBuffer.Size());
        int err = on_frame(buf, FrameBuffer.Size(), i, user_data);
        if (err) {
            fprintf(stderr, "break shit\n");
            break;
        }
    }

    fprintf(stderr, "done reading pcm\n");
    return result;
}

Result_t read_JP2K_file(asset_t *asset, asdcp_on_j2k_frame_func on_frame, void *user_data)
{
    AESDecContext* Context = 0;
    HMACContext* HMAC = 0;
    AS_02::JP2K::MXFReader Reader;
    JP2K::FrameBuffer FrameBuffer(FRAME_BUFFER_SIZE);
    ui32_t frame_count = 0;

    Result_t result = Reader.OpenRead(asset->mxf_path);

    if (ASDCP_SUCCESS(result)) {
        ASDCP::MXF::RGBAEssenceDescriptor *rgba_descriptor = 0;
        ASDCP::MXF::CDCIEssenceDescriptor *cdci_descriptor = 0;

        result = Reader.OP1aHeader().GetMDObjectByType(DefaultCompositeDict().ul(MDD_RGBAEssenceDescriptor),
                reinterpret_cast<MXF::InterchangeObject**>(&rgba_descriptor));

        if (KM_SUCCESS(result)) {
            assert(rgba_descriptor);
            frame_count = (ui32_t)rgba_descriptor->ContainerDuration;
        } else {
            result = Reader.OP1aHeader().GetMDObjectByType(DefaultCompositeDict().ul(MDD_CDCIEssenceDescriptor),
                    reinterpret_cast<MXF::InterchangeObject**>(&cdci_descriptor));

            if (KM_SUCCESS(result)) {
                assert(cdci_descriptor);
                frame_count = (ui32_t)cdci_descriptor->ContainerDuration;
            } else {
                fprintf(stderr, "File does not contain an essence descriptor.\n");
                frame_count = Reader.AS02IndexReader().GetDuration();
            }
        }

        if (frame_count == 0) {
            frame_count = Reader.AS02IndexReader().GetDuration();
        }

        if (frame_count == 0) {
            fprintf(stderr, "Unable to determine file duration.\n");
            return RESULT_FAIL;
        }
    }

    /*
       if (ASDCP_SUCCESS(result) && Options.key_flag)
       {
       Context = new AESDecContext;
       result = Context->InitKey(Options.key_value);

       if ( ASDCP_SUCCESS(result) && Options.read_hmac )
       {
       WriterInfo Info;
       Reader.FillWriterInfo(Info);

       if ( Info.UsesHMAC )
       {
       HMAC = new HMACContext;
       result = HMAC->InitKey(Options.key_value, Info.LabelSetType);
       }
       else
       {
       fputs("File does not contain HMAC values, ignoring -m option.\n", stderr);
       }
       }
       }
       */

    unsigned int start_frame = asset->start_frame;
    unsigned int last_frame = asset->end_frame;

    fprintf(stderr, "decode %s [%d, %d[\n", asset->mxf_path, start_frame, last_frame);
    for (int i = start_frame; i < last_frame; i++) {
        result = Reader.ReadFrame(i, FrameBuffer, Context, HMAC);

        if (ASDCP_SUCCESS(result)) {
            unsigned char *buf = (unsigned char*)malloc(FrameBuffer.Size());
            memcpy(buf, FrameBuffer.Data(), FrameBuffer.Size());
            int err = on_frame(buf, FrameBuffer.Size(), i, user_data);
            if (err) {
                break;
            }
        } else {
            break;
        }
    } 

    return result;
}

int asdcp_read_audio_files(linked_list_t *files, asdcp_on_pcm_frame_func on_frame, void *user_data) { int err = 0;

    for (linked_list_t *c = files; !err && c; c = c->next) {
        EssenceType_t essenceType;
        asset_t *asset = (asset_t*)c->user_data;
        Result_t result = ASDCP::EssenceType(asset->mxf_path, essenceType);

        if (!ASDCP_SUCCESS(result)) {
            err = 1;
            break;
        }
        if (essenceType != ESS_AS02_PCM_24b_48k && essenceType != ESS_AS02_PCM_24b_96k) {
            fprintf(stderr, "invalid audio essence supported for now\n");
            err = 1;
            break;
        }
        result = read_PCM_file(asset, on_frame, user_data);
        if (!ASDCP_SUCCESS(result)) {
            err = 1;
            break;
        }
    }
    on_frame(NULL, 0, 0, user_data);

    return !err;
}

int asdcp_read_video_files(linked_list_t *files, asdcp_on_j2k_frame_func on_frame, void *user_data) {
    int err = 0;

    for (linked_list_t *c = files; !err && c; c = c->next) {
        EssenceType_t essenceType;
        asset_t *asset = (asset_t*)c->user_data;
        Result_t result = ASDCP::EssenceType(asset->mxf_path, essenceType);

        if (!ASDCP_SUCCESS(result)) {
            err = 1;
            break;
        }
        if (essenceType != ESS_AS02_JPEG_2000) {
            fprintf(stderr, "only jpeg2000 supported for now\n");
            err = 1;
            break;
        }
        result = read_JP2K_file(asset, on_frame, user_data);
        if (!ASDCP_SUCCESS(result)) {
            err = 1;
            break;
        }
    }

    fprintf(stderr, "shutdown frame\n");
    on_frame(NULL, 0, 0, user_data);

    return !err;
}
