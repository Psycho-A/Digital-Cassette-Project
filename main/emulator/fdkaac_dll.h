/* fdkaac_dll.h */
/* ------------------
 * AAC-кодер FDK-AAC:
 * Указатели на базовые функции
 * Взято из aacenc_lib.h
 */
#ifndef FDKAAC_DLL_H
#define FDKAAC_DLL_H
#include <windows.h>

/* Непрозрачный хэндл кодера */
typedef void* HANDLE_AACENCODER;

/* Параметры кодера (все) */
typedef enum {
    AACENC_AOT               = 0x0100,  /* Audio object type (codec profile).
                                           - 2: MPEG-4 AAC Low Complexity.
                                           - 5: MPEG-4 AAC Low Complexity with Spectral Band Replication (HE-AAC).
                                           - 29: MPEG-4 AAC Low Complexity with Spectral Band Replication and Parametric Stereo (HE-AAC v2).
                                                 This configuration can be used only with stereo input audio data.
                                           - 23: MPEG-4 AAC Low-Delay.
                                           - 39: MPEG-4 AAC Enhanced Low-Delay. Since there is no ::AUDIO_OBJECT_TYPE for ELD in
                                                 combination with SBR defined, enable SBR explicitely by ::AACENC_SBR_MODE parameter. */

    AACENC_BITRATE           = 0x0101,  /* Total encoder bitrate. This parameter is mandatory and interacts with ::AACENC_BITRATEMODE.
                                           - CBR: Bitrate in bits/second.
                                             See \ref suppBitrates for details. */

    AACENC_BITRATEMODE       = 0x0102,  /* Bitrate mode. Configuration can be different kind of bitrate configurations:
                                           - 0: Constant bitrate, use bitrate according to ::AACENC_BITRATE. (default)
                                                Within none LD/ELD ::AUDIO_OBJECT_TYPE, the CBR mode makes use of full allowed bitreservoir.
                                                In contrast, at Low-Delay ::AUDIO_OBJECT_TYPE the bitreservoir is kept very small.
                                           - 8: LD/ELD full bitreservoir for packet based transmission. */

    AACENC_SAMPLERATE        = 0x0103,  /* Audio input data sampling rate. Encoder supports following sampling rates:
                                           8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 64000, 88200, 96000 */

    AACENC_SBR_MODE          = 0x0104,  /* Configure SBR independently of the chosen Audio Object Type ::AUDIO_OBJECT_TYPE.
                                           This parameter is for ELD audio object type only.
                                           - -1: Use ELD SBR auto configurator (default).
                                           - 0: Disable Spectral Band Replication.
                                           - 1: Enable Spectral Band Replication. */

    AACENC_GRANULE_LENGTH    = 0x0105,  /* Core encoder (AAC) audio frame length in samples:
                                           - 1024: Default configuration.
                                           - 512: Default LD/ELD configuration.
                                           - 480: Optional length in LD/ELD configuration. */

    AACENC_CHANNELMODE       = 0x0106,  /* Set explicit channel mode. Channel mode must match with number of input channels.
                                           - 1-7 and 33,34: MPEG channel modes supported, see ::CHANNEL_MODE in FDK_audio.h. */

    AACENC_CHANNELORDER      = 0x0107,  /* Input audio data channel ordering scheme:
                                           - 0: MPEG channel ordering (e. g. 5.1: C, L, R, SL, SR, LFE). (default)
                                           - 1: WAVE file format channel ordering (e. g. 5.1: L, R, C, LFE, SL, SR). */

    AACENC_SBR_RATIO         = 0x0108,  /*  Controls activation of downsampled SBR. With downsampled SBR, the delay will be
                                            shorter. On the other hand, for achieving the same quality level, downsampled SBR
                                            needs more bits than dual-rate SBR.
                                            With downsampled SBR, the AAC encoder will work at the same sampling rate as the
                                            SBR encoder (single rate).
                                            Downsampled SBR is supported for AAC-ELD and HE-AACv1.
                                            - 1: Downsampled SBR (default for ELD).
                                            - 2: Dual-rate SBR   (default for HE-AAC). */

    AACENC_AFTERBURNER       = 0x0200,  /* This parameter controls the use of the afterburner feature.
                                           The afterburner is a type of analysis by synthesis algorithm which increases the
                                           audio quality but also the required processing power. It is recommended to always
                                           activate this if additional memory consumption and processing power consumption
                                           is not a problem. If increased MHz and memory consumption are an issue then the MHz
                                           and memory cost of this optional module need to be evaluated against the improvement
                                           in audio quality on a case by case basis.
                                           - 0: Disable afterburner (default).
                                           - 1: Enable afterburner. */

    AACENC_BANDWIDTH         = 0x0203,  /* Core encoder audio bandwidth:
                                           - 0: Determine bandwidth internally (default, see chapter \ref BEHAVIOUR_BANDWIDTH).
                                           - 1 to fs/2: Frequency bandwidth in Hertz. (Experts only, better do not
                                                        touch this value to avoid degraded audio quality) */

    AACENC_PEAK_BITRATE      = 0x0207,  /* Peak bitrate configuration parameter to adjust maximum bits per audio frame. Bitrate is in bits/second. 
                                           The peak bitrate will internally be limited to the chosen bitrate ::AACENC_BITRATE as lower limit
                                           and the number_of_effective_channels*6144 bit as upper limit.

                                           Setting the peak bitrate equal to ::AACENC_BITRATE does not necessarily mean that the audio frames
                                           will be of constant size. Since the peak bitate is in bits/second, the frame sizes can vary by
                                           one byte in one or the other direction over various frames. However, it is not recommended to reduce
                                           the peak pitrate to ::AACENC_BITRATE - it would disable the bitreservoir, which would affect the
                                           audio quality by a large amount. */

    AACENC_TRANSMUX          = 0x0300,  /* Transport type to be used. See ::TRANSPORT_TYPE in FDK_audio.h. Following
                                           types can be configured in encoder library:
                                           - 0: raw access units
                                           - 1: ADIF bitstream format
                                           - 2: ADTS bitstream format
                                           - 6: Audio Mux Elements (LATM) with muxConfigPresent = 1
                                           - 7: Audio Mux Elements (LATM) with muxConfigPresent = 0, out of band StreamMuxConfig
                                           - 10: Audio Sync Stream (LOAS) */

    AACENC_HEADER_PERIOD     = 0x0301,  /* Frame count period for sending in-band configuration buffers within LATM/LOAS
                                           transport layer. Additionally this parameter configures the PCE repetition period
                                           in raw_data_block(). See \ref encPCE.
                                           - 0xFF: auto-mode default 10 for TT_MP4_ADTS, TT_MP4_LOAS and TT_MP4_LATM_MCP1, otherwise 0.
                                           - n: Frame count period. */

    AACENC_SIGNALING_MODE    = 0x0302,  /* Signaling mode of the extension AOT:
                                           - 0: Implicit backward compatible signaling (default for non-MPEG-4 based
                                                AOT's and for the transport formats ADIF and ADTS)
                                                - A stream that uses implicit signaling can be decoded by every AAC decoder, even AAC-LC-only decoders
                                                - An AAC-LC-only decoder will only decode the low-frequency part of the stream, resulting in a band-limited output
                                                - This method works with all transport formats
                                                - This method does not work with downsampled SBR
                                           - 1: Explicit backward compatible signaling
                                                - A stream that uses explicit backward compatible signaling can be decoded by every AAC decoder, even AAC-LC-only decoders
                                                - An AAC-LC-only decoder will only decode the low-frequency part of the stream, resulting in a band-limited output
                                                - A decoder not capable of decoding PS will only decode the AAC-LC+SBR part.
                                                  If the stream contained PS, the result will be a a decoded mono downmix
                                                - This method does not work with ADIF or ADTS. For LOAS/LATM, it only works with AudioMuxVersion==1
                                                - This method does work with downsampled SBR
                                           - 2: Explicit hierarchical signaling (default for MPEG-4 based AOT's and for all transport formats excluding ADIF and ADTS)
                                                - A stream that uses explicit hierarchical signaling can be decoded only by HE-AAC decoders
                                                - An AAC-LC-only decoder will not decode a stream that uses explicit hierarchical signaling
                                                - A decoder not capable of decoding PS will not decode the stream at all if it contained PS
                                                - This method does not work with ADIF or ADTS. It works with LOAS/LATM and the MPEG-4 File format
                                                - This method does work with downsampled SBR

                                            For making sure that the listener always experiences the best audio quality,
                                            explicit hierarchical signaling should be used.
                                            This makes sure that only a full HE-AAC-capable decoder will decode those streams.
                                            The audio is played at full bandwidth.
                                            For best backwards compatibility, it is recommended to encode with implicit SBR signaling.
                                            A decoder capable of AAC-LC only will then only decode the AAC part, which means the decoded
                                            audio will sound band-limited.

                                            For MPEG-2 transport types (ADTS,ADIF), only implicit signaling is possible.

                                            For LOAS and LATM, explicit backwards compatible signaling only works together with AudioMuxVersion==1.
                                            The reason is that, for explicit backwards compatible signaling, additional information will be appended to the ASC.
                                            A decoder that is only capable of decoding AAC-LC will skip this part.
                                            Nevertheless, for jumping to the end of the ASC, it needs to know the ASC length.
                                            Transmitting the length of the ASC is a feature of AudioMuxVersion==1, it is not possible to transmit the
                                            length of the ASC with AudioMuxVersion==0, therefore an AAC-LC-only decoder will not be able to parse a
                                            LOAS/LATM stream that was being encoded with AudioMuxVersion==0.

                                            For downsampled SBR, explicit signaling is mandatory. The reason for this is that the
                                            extension sampling frequency (which is in case of SBR the sampling frequqncy of the SBR part)
                                            can only be signaled in explicit mode.

                                            For AAC-ELD, the SBR information is transmitted in the ELDSpecific Config, which is part of the
                                            AudioSpecificConfig. Therefore, the settings here will have no effect on AAC-ELD.*/

    AACENC_TPSUBFRAMES       = 0x0303,  /* Number of sub frames in a transport frame for LOAS/LATM or ADTS (default 1).
                                           - ADTS: Maximum number of sub frames restricted to 4.
                                           - LOAS/LATM: Maximum number of sub frames restricted to 2.*/

    AACENC_AUDIOMUXVER       = 0x0304,  /* AudioMuxVersion to be used for LATM. (AudioMuxVersionA, currently not implemented):
                                           - 0: Default, no transmission of tara Buffer fullness, no ASC length and including actual latm Buffer fullnes.
                                           - 1: Transmission of tara Buffer fullness, ASC length and actual latm Buffer fullness.
                                           - 2: Transmission of tara Buffer fullness, ASC length and maximum level of latm Buffer fullness. */

    AACENC_PROTECTION        = 0x0306,  /* Configure protection in tranpsort layer:
                                           - 0: No protection. (default)
                                           - 1: CRC active for ADTS bitstream format. */

    AACENC_ANCILLARY_BITRATE = 0x0500,  /* Constant ancillary data bitrate in bits/second.
                                           - 0: Either no ancillary data or insert exact number of bytes, denoted via
                                                input parameter, numAncBytes in AACENC_InArgs.
                                           - else: Insert ancillary data with specified bitrate. */

    AACENC_METADATA_MODE     = 0x0600,  /* Configure Meta Data. See ::AACENC_MetaData for further details:
                                           - 0: Do not embed any metadata.
                                           - 1: Embed MPEG defined metadata only.
                                           - 2: Embed all metadata. */

    AACENC_CONTROL_STATE     = 0xFF00,  /* There is an automatic process which internally reconfigures the encoder instance
                                           when a configuration parameter changed or an error occured. This paramerter allows
                                           overwriting or getting the control status of this process. See AACENC_CTRLFLAGS enum. */

    AACENC_NONE              = 0xFFFF   /* Nothing. */

} AACENC_Param;


/* Информация о кодере (возвращается aacEncInfo) */
typedef struct {
    unsigned int  maxOutBufBytes;      // максимальный размер выходного буфера (на один вызов encode)
    unsigned int  maxAncBytes;         // максимальный размер дополнительных данных (обычно 0)
    unsigned int  inBufFillLevel;      // заполненность внутреннего буфера (в сэмплах на канал)
    unsigned int  inputChannels;       // ожидаемое число входных каналов во время декодирования
    unsigned int  frameLength;         // длина фрейма в сэмплах на канал (1024 для AAC-LC, 2048 для HE-AAC)
    unsigned int  encoderDelay;        // задержка кодера в сэмплах (на канал)
    unsigned char confBuf[64];         // буфер конфигурации AudioSpecificConfig (максимум 64 байта)
    unsigned int  confSize;            // размер конфигурационных данных в confBuf (в байтах)
} AACENC_Info;


/* Структура для передачи входных/выходных буферов */
typedef struct {
    int    numBufs;            // количество буферов (обычно 1)
    void **bufs;               // массив указателей на буферы
    int   *bufferIdentifiers;  // идентификаторы буферов (из AACENC_BufferIdentifier)
    int   *bufSizes;           // размеры буферов в байтах
    int   *bufElSizes;         // размер элемента (для PCM = 2)
} AACENC_BufDesc;

typedef enum {
    IN_AUDIO_DATA      = 0,    // Audio input buffer, interleaved INT_PCM samples.
    IN_ANCILLRY_DATA   = 1,    // Ancillary data to be embedded into bitstream.
    IN_METADATA_SETUP  = 2,    // Setup structure for embedding meta data.
    OUT_BITSTREAM_DATA = 3,    // Buffer holds bitstream output data.
    OUT_AU_SIZES       = 4     // Buffer contains sizes of each access unit.
} AACENC_BufferIdentifier;


/* Структура входных аргументов aacEncEncode */
typedef struct {
    int numInSamples;          // количество валидных сэмплов во входном буфере
    int numAncBytes;           // количество дополнительных данных (0)
} AACENC_InArgs;

/* Структура выходных аргументов aacEncEncode */
typedef struct {
    int numOutBytes;           // количество записанных байт в выходном буфере
    int numInSamples;          // количество потреблённых входных сэмплов
    int numAncBytes;           // количество потреблённых дополнительных данных
} AACENC_OutArgs;


/* Перечисление возможных ошибок */
typedef enum {
    AACENC_OK                    = 0x0000,  // No error happened. All fine.
    AACENC_INVALID_HANDLE        = 0x0020,  // Handle passed to function call was invalid.
    AACENC_MEMORY_ERROR          = 0x0021,  // Memory allocation failed.
    AACENC_UNSUPPORTED_PARAMETER = 0x0022,  // Parameter not available.
    AACENC_INVALID_CONFIG        = 0x0023,  // Configuration not provided.
    AACENC_INIT_ERROR            = 0x0040,  // General initialization error.
    AACENC_INIT_AAC_ERROR        = 0x0041,  // AAC library initialization error.
    AACENC_INIT_SBR_ERROR        = 0x0042,  // SBR library initialization error.
    AACENC_INIT_TP_ERROR         = 0x0043,  // Transport library initialization error.
    AACENC_INIT_META_ERROR       = 0x0044,  // Meta data library initialization error.
    AACENC_ENCODE_ERROR          = 0x0060,  // The encoding process was interrupted by an unexpected error.
    AACENC_ENCODE_EOF            = 0x0080   // End of file reached.
} AACENC_ERROR;


/* Типы указателей на функции */
typedef AACENC_ERROR (__cdecl *LPFDK_OPEN)(HANDLE_AACENCODER *phAacEncoder, unsigned int encModules, unsigned int maxChannels);  // не использовать WINAPI, __cdecl можно опустить
typedef AACENC_ERROR (__cdecl *LPFDK_SET_PARAM)(HANDLE_AACENCODER hAacEncoder, AACENC_Param param, unsigned int value);
typedef AACENC_ERROR (__cdecl *LPFDK_GET_PARAM)(HANDLE_AACENCODER hAacEncoder, AACENC_Param param);  // не используется?
typedef AACENC_ERROR (__cdecl *LPFDK_INFO)(HANDLE_AACENCODER hAacEncoder, AACENC_Info *pInfo);
typedef AACENC_ERROR (__cdecl *LPFDK_ENCODE)(HANDLE_AACENCODER hAacEncoder, AACENC_BufDesc *inBufDesc, AACENC_BufDesc *outBufDesc, AACENC_InArgs *inargs, AACENC_OutArgs *outargs);
typedef AACENC_ERROR (__cdecl *LPFDK_CLOSE)(HANDLE_AACENCODER *phAacEncoder);


/* Глобальные переменные */
extern HMODULE           g_hFdkAac;
extern LPFDK_OPEN        g_fdkOpen;
extern LPFDK_SET_PARAM   g_fdkSetParam;
extern LPFDK_GET_PARAM   g_fdkGetParam;
extern LPFDK_INFO        g_fdkInfo;
extern LPFDK_ENCODE      g_fdkEncode;
extern LPFDK_CLOSE       g_fdkClose;


/* Загрузка и выгрузка DLL */
int load_fdkaac_dll(void);
int free_fdkaac_dll(void);


#endif