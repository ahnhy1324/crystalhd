// SPDX-License-Identifier: GPL-2.0-or-later
/* OWN parser controls only; no FFmpeg, media, decoder, card or AU validity. */
#include "vaapi-packet-admission-nal-ref.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
static const uint8_t annexb_context[]={0,0,1,0x67,0x80,0,0,1,0x68,0x80};
static void require(bool condition, const char *message)
{
    ++checks;
    if (!condition) { fprintf(stderr,"NAL-ref control FAIL: %s\n",message); exit(1); }
}
static struct crystalhd_nal_ref parse(const uint8_t *bytes, int size)
{
    struct crystalhd_nal_ref r=crystalhd_nal_ref_packet(bytes,size,
        annexb_context,sizeof(annexb_context),CRYSTALHD_NAL_OK);
    unsigned total=0, refs=0;
    for (unsigned i=0;i<32;++i) total+=r.nal_type_counts[i];
    for (unsigned i=0;i<4;++i) refs+=r.vcl_ref_idc_counts[i];
    require(total==r.nal_count && refs==r.vcl_count &&
            r.vcl_count==r.nal_type_counts[1]+r.nal_type_counts[5],"partial and complete counters agree");
    require(r.nal_count<=CRYSTALHD_NAL_REF_MAX_NALS && r.bytes_scanned<=CRYSTALHD_NAL_REF_MAX_BYTES &&
            (size<0?r.bytes_scanned==0:r.bytes_scanned<=(unsigned)size),"bounded reads/counters");
    return r;
}
static void unknown(const uint8_t *bytes, int size, enum crystalhd_nal_ref_reason reason)
{
    const struct crystalhd_nal_ref r=parse(bytes,size);
    require(r.classification==CRYSTALHD_NAL_UNKNOWN && r.reason==reason &&
            r.framing_complete==(reason==CRYSTALHD_NAL_NO_VCL),"explicit unknown reason, never a reference guess");
}
int main(void)
{
    uint8_t zero[]={0,0,1,1,0xac}, before[sizeof(zero)];
    memcpy(before,zero,sizeof(zero));
    struct crystalhd_nal_ref r=parse(zero,sizeof(zero));
    require(r.classification==CRYSTALHD_NAL_ALL_ZERO && r.reason==CRYSTALHD_NAL_OK &&
            r.framing_complete && r.nal_count==1 && r.vcl_ref_idc_counts[0]==1 &&
            r.context_annex_b && r.bytes_scanned==sizeof(zero) &&
            !memcmp(zero,before,sizeof(zero)),"known context, three-byte start and immutable input");
    const uint8_t idr[]={0,0,0,1,0x65,0x80};
    r=parse(idr,sizeof(idr));
    require(r.classification==CRYSTALHD_NAL_ALL_REF && r.nal_type_counts[5]==1 &&
            r.vcl_ref_idc_counts[3]==1,"four-byte IDR with nonzero reference header");
    const uint8_t padded[]={0,0,0,0,1,1,0x80,0,0,0,1,1,0x80,0,0,0};
    r=parse(padded,sizeof(padded));
    require(r.classification==CRYSTALHD_NAL_ALL_ZERO && r.nal_count==2 && r.vcl_count==2,
            "leading/trailing zero bytes and multiple VCL headers");
    const uint8_t nonvcl_refs[]={0,0,1,0x67,0x80,0,0,1,0x66,0x80,0,0,1,1,0x80};
    r=parse(nonvcl_refs,sizeof(nonvcl_refs));
    require(r.classification==CRYSTALHD_NAL_ALL_ZERO && r.nal_count==3 && r.vcl_count==1 &&
            r.nal_type_counts[7]==1 && r.nal_type_counts[6]==1 && r.vcl_ref_idc_counts[0]==1,
            "SPS/SEI header ref bits never enter the VCL histogram; no semantic validity claim");
    const uint8_t reference_mix[]={0,0,1,0x21,0x80,0,0,1,0x41,0x80,0,0,1,0x61,0x80};
    r=parse(reference_mix,sizeof(reference_mix));
    require(r.classification==CRYSTALHD_NAL_ALL_REF && r.vcl_count==3 &&
            r.vcl_ref_idc_counts[1]==1 && r.vcl_ref_idc_counts[2]==1 && r.vcl_ref_idc_counts[3]==1,
            "all nonzero ref_idc values count as reference, not just value3");
    /* Synthetic first_mb UE0 / slice_type UE1(B) / PPS UE0 prefix. It is not
     * decoded or an AU golden; a B label cannot override this reference NAL. */
    const uint8_t reference_b[]={0,0,1,0x61,0xac};
    r=parse(reference_b,sizeof(reference_b));
    require(r.classification==CRYSTALHD_NAL_ALL_REF && r.vcl_ref_idc_counts[3]==1,
            "reference-capable B-prefix counterexample is not called non-reference");
    const uint8_t mixed[]={0,0,1,1,0x80,0,0,1,0x41,0x80};
    r=parse(mixed,sizeof(mixed));
    require(r.classification==CRYSTALHD_NAL_MIXED && r.vcl_ref_idc_counts[0]==1 &&
            r.vcl_ref_idc_counts[2]==1,"mixed VCL reference states remain mixed");
    const uint8_t escaped[]={0,0,1,1,0,0,3,1,0x80};
    r=parse(escaped,sizeof(escaped));
    require(r.classification==CRYSTALHD_NAL_ALL_ZERO && r.nal_count==1,
            "legal emulation prevention does not create another NAL");
    unknown(NULL,0,CRYSTALHD_NAL_NO_DATA);
    unknown(NULL,7,CRYSTALHD_NAL_NO_DATA);
    unknown(zero,-1,CRYSTALHD_NAL_INVALID_SIZE);
    const uint8_t avcc[]={0,0,0,2,1,0x80}, junk[]={0xff,0,0,1,1,0x80};
    unknown(avcc,sizeof(avcc),CRYSTALHD_NAL_NOT_ANNEX_B);
    unknown(junk,sizeof(junk),CRYSTALHD_NAL_NOT_ANNEX_B);
    const uint8_t zeros[]={0,0,0,0}, prefix[]={0,0,1}, header[]={0,0,1,1};
    unknown(zeros,sizeof(zeros),CRYSTALHD_NAL_NOT_ANNEX_B);
    unknown(prefix,sizeof(prefix),CRYSTALHD_NAL_TRUNCATED);
    unknown(header,sizeof(header),CRYSTALHD_NAL_TRUNCATED);
    const uint8_t empty[]={0,0,1,0,0,1,1,0x80}, tail[]={0,0,1,1,0x80,0,0,1};
    unknown(empty,sizeof(empty),CRYSTALHD_NAL_TRUNCATED);
    unknown(tail,sizeof(tail),CRYSTALHD_NAL_TRUNCATED);
    const uint8_t forbidden[]={0,0,1,0x81,0x80}, idr_zero[]={0,0,1,5,0x80};
    unknown(forbidden,sizeof(forbidden),CRYSTALHD_NAL_FORBIDDEN_BIT);
    unknown(idr_zero,sizeof(idr_zero),CRYSTALHD_NAL_IDR_REF_ZERO);
    for (unsigned type=0;type<32;++type) {
        if (type==1 || type==5 || (type>=6 && type<=12)) continue;
        uint8_t unsupported[]={0,0,1,(uint8_t)(0x60U|type),0x80};
        unknown(unsupported,sizeof(unsupported),CRYSTALHD_NAL_UNSUPPORTED_TYPE);
    }
    const uint8_t epb_tail[]={0,0,1,1,0,0,3};
    const uint8_t epb_wrong[]={0,0,1,1,0,0,3,4,0x80}, unescaped[]={0,0,1,1,0,0,2,0x80};
    unknown(epb_tail,sizeof(epb_tail),CRYSTALHD_NAL_BAD_ESCAPE);
    unknown(epb_wrong,sizeof(epb_wrong),CRYSTALHD_NAL_BAD_ESCAPE);
    unknown(unescaped,sizeof(unescaped),CRYSTALHD_NAL_BAD_ESCAPE);
    const uint8_t no_vcl[]={0,0,1,9,0xf0,0,0,1,0x67,0x80,0,0,1,0x68,0x80};
    unknown(no_vcl,sizeof(no_vcl),CRYSTALHD_NAL_NO_VCL);
    static uint8_t too_large[CRYSTALHD_NAL_REF_MAX_BYTES+1U];
    r=parse(too_large,sizeof(too_large));
    require(r.reason==CRYSTALHD_NAL_BYTE_CAP && r.classification==CRYSTALHD_NAL_UNKNOWN &&
            r.bytes_scanned==0,"oversize never silently classifies a prefix or reads beyond its cap");
    uint8_t many[(CRYSTALHD_NAL_REF_MAX_NALS+1U)*5U];
    for (unsigned i=0;i<CRYSTALHD_NAL_REF_MAX_NALS+1U;++i) {
        memcpy(many+i*5U,zero,5);
    }
    r=parse(many,CRYSTALHD_NAL_REF_MAX_NALS*5U);
    require(r.classification==CRYSTALHD_NAL_ALL_ZERO && r.nal_count==CRYSTALHD_NAL_REF_MAX_NALS,
            "exact NAL count boundary remains complete");
    r=parse(many,sizeof(many));
    require(r.classification==CRYSTALHD_NAL_UNKNOWN && r.reason==CRYSTALHD_NAL_NAL_CAP &&
            !r.framing_complete && r.nal_count==CRYSTALHD_NAL_REF_MAX_NALS,
            "one additional NAL is unknown, never a complete partial histogram");
    /* A four-byte AVCC length257 looks like a three-byte Annex B prefix plus
     * nonref header1, although the actual following NAL header is REF0x41.
     * Length353 similarly looks like reference header0x61 before a real ref0
     * NAL. Only authoritative context framing may select the packet parser. */
    static uint8_t collision257[4+257]={0,0,1,1,0x41,0x80};
    static uint8_t collision353[4+353]={0,0,1,0x61,1,0x80};
    const uint8_t avcc_context[]={1,100,0,31,0xff};
    r=crystalhd_nal_ref_annexb(collision257,sizeof(collision257));
    require(r.classification==CRYSTALHD_NAL_ALL_ZERO,
            "raw prefix-only counterexample proves why packet bytes are not framing authority");
    r=crystalhd_nal_ref_packet(collision257,sizeof(collision257),avcc_context,sizeof(avcc_context),CRYSTALHD_NAL_OK);
    require(r.classification==CRYSTALHD_NAL_UNKNOWN && r.reason==CRYSTALHD_NAL_CONTEXT_AVCC &&
            !r.context_annex_b && !r.bytes_scanned && !r.nal_count,"AVCC257 collision never becomes ALL_VCL_ZERO");
    r=crystalhd_nal_ref_packet(collision353,sizeof(collision353),avcc_context,sizeof(avcc_context),CRYSTALHD_NAL_OK);
    require(r.classification==CRYSTALHD_NAL_UNKNOWN && r.reason==CRYSTALHD_NAL_CONTEXT_AVCC &&
            !r.bytes_scanned,"other AVCC length/header collisions are not guessed");
    r=crystalhd_nal_ref_packet(zero,sizeof(zero),NULL,0,CRYSTALHD_NAL_OK);
    require(r.reason==CRYSTALHD_NAL_CONTEXT_MISSING && !r.context_annex_b && !r.bytes_scanned,
            "start-code-looking payload does not repair absent framing authority");
    r=crystalhd_nal_ref_packet(zero,sizeof(zero),annexb_context,5,CRYSTALHD_NAL_OK);
    require(r.reason==CRYSTALHD_NAL_CONTEXT_AMBIGUOUS && !r.context_annex_b && !r.bytes_scanned,
            "SPS-only or ambiguous extradata does not establish the gated mode");
    for (unsigned reason=CRYSTALHD_NAL_CONTEXT_AVCC;reason<=CRYSTALHD_NAL_OPTIONS_NONZERO;++reason) {
        r=crystalhd_nal_ref_packet(zero,sizeof(zero),annexb_context,sizeof(annexb_context),
                                  (enum crystalhd_nal_ref_reason)reason);
        require(r.classification==CRYSTALHD_NAL_UNKNOWN && r.reason==(enum crystalhd_nal_ref_reason)reason &&
                !r.context_annex_b && !r.bytes_scanned && !r.nal_count,
                "unknown open/transition authority cannot be upgraded by current Annex B bytes");
    }
    printf("OWN Annex B NAL-ref controls PASS (%u checks); no slice/AU/decoder qualification\n",checks);
    return 0;
}
