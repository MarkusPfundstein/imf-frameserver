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


// cp 
// Metadata.h -> /usr/local/include
// MXF.h /usr/local/include/
// MXFTypes.h /usr/local/include/
// KLV.h /usr/local/include/
// MDD.h /usr/local/include/
// WavFileWriter.h /usr/local/include/
// Wav.h /usr/local/include/

Result_t read_JP2K_file(const char *filename, void *user_data, asdcp_on_j2k_frame_func on_frame)
{
  AESDecContext* Context = 0;
  HMACContext* HMAC = 0;
  AS_02::JP2K::MXFReader Reader;
  JP2K::FrameBuffer FrameBuffer(FRAME_BUFFER_SIZE);
  ui32_t frame_count = 0;

  Result_t result = Reader.OpenRead(filename);

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

  ui32_t start_frame = 0;
  ui32_t last_frame = frame_count;
  //ui32_t last_frame = Options.start_frame + ( Options.duration ? Options.duration : frame_count);
  if (last_frame > frame_count) {
    last_frame = frame_count;
  }

  for (ui32_t i = start_frame; ASDCP_SUCCESS(result) && i < last_frame; i++) {
    result = Reader.ReadFrame(i, FrameBuffer, Context, HMAC);

    if (ASDCP_SUCCESS(result)) {
      unsigned char *buf = (unsigned char*)malloc(FrameBuffer.Size());
      memcpy(buf, FrameBuffer.Data(), FrameBuffer.Size());
      int err = on_frame(buf, FrameBuffer.Size(), i, user_data);
      if (err) {
        break;
      }
    }
  } 
  // tell we are done here
  on_frame(NULL, 0, last_frame, user_data);

  return result;
}



int asdcp_read_mxf(const char *filename, void *user_data, asdcp_on_j2k_frame_func on_frame) {
  EssenceType_t essenceType;
  fprintf(stderr, "%s\n", filename);
  Result_t result = ASDCP::EssenceType(filename, essenceType);

  if (ASDCP_SUCCESS(result)) {
    switch (essenceType) {
      case ESS_AS02_JPEG_2000:
        result = read_JP2K_file(filename, user_data, on_frame);
        break;
      case ESS_AS02_ACES:
        //result = read_ACES_file(Options);
        break;
      case ESS_AS02_PCM_24b_48k:
      case ESS_AS02_PCM_24b_96k:
        //result = read_PCM_file(Options);
        break;
      case ESS_AS02_TIMED_TEXT:
        //result = read_timed_text_file(Options);
        break;
      default:
        fprintf(stderr, "%s: Unknown file type (%d), not AS-02 essence.\n", filename, essenceType);
        return 1;
    }
  }

  if (ASDCP_FAILURE(result)) {
    return 1;
  }
  return 0;
}
