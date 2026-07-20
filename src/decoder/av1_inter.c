#include "av1_inter.h"
#include "base.h"

void av1_inter_cdfs_init(Av1InterCdfs *cdfs) {
    static const uint16_t is_inter[4][3] = {
        { 806U, 32768U, 0U }, { 16662U, 32768U, 0U },
        { 20186U, 32768U, 0U }, { 26538U, 32768U, 0U }
    };
    static const uint16_t skip_mode[3][3] = {
        { 32621U, 32768U, 0U }, { 20708U, 32768U, 0U },
        { 8127U, 32768U, 0U }
    };
    static const uint16_t compound_reference[5][3] = {
        { 26828U, 32768U, 0U }, { 24035U, 32768U, 0U },
        { 12031U, 32768U, 0U }, { 10640U, 32768U, 0U },
        { 2901U, 32768U, 0U }
    };
    static const uint16_t compound_reference_type[5][3] = {
        { 1198U, 32768U, 0U }, { 2070U, 32768U, 0U },
        { 9166U, 32768U, 0U }, { 7499U, 32768U, 0U },
        { 22475U, 32768U, 0U }
    };
    static const uint16_t single_reference[3][6][3] = {
        { { 4897U, 32768U, 0U }, { 1555U, 32768U, 0U },
          { 4236U, 32768U, 0U }, { 8650U, 32768U, 0U },
          { 904U, 32768U, 0U }, { 1444U, 32768U, 0U } },
        { { 16973U, 32768U, 0U }, { 16751U, 32768U, 0U },
          { 19647U, 32768U, 0U }, { 24773U, 32768U, 0U },
          { 11014U, 32768U, 0U }, { 15087U, 32768U, 0U } },
        { { 29744U, 32768U, 0U }, { 30279U, 32768U, 0U },
          { 31194U, 32768U, 0U }, { 31895U, 32768U, 0U },
          { 26875U, 32768U, 0U }, { 30304U, 32768U, 0U } }
    };
    static const uint16_t unidirectional_reference[3][3][3] = {
        { { 5284U, 32768U, 0U }, { 3865U, 32768U, 0U },
          { 3128U, 32768U, 0U } },
        { { 23152U, 32768U, 0U }, { 14173U, 32768U, 0U },
          { 15270U, 32768U, 0U } },
        { { 31774U, 32768U, 0U }, { 25120U, 32768U, 0U },
          { 26710U, 32768U, 0U } }
    };
    static const uint16_t compound_forward_reference[3][3][3] = {
        { { 4946U, 32768U, 0U }, { 9468U, 32768U, 0U },
          { 1503U, 32768U, 0U } },
        { { 19891U, 32768U, 0U }, { 22441U, 32768U, 0U },
          { 15160U, 32768U, 0U } },
        { { 30731U, 32768U, 0U }, { 31059U, 32768U, 0U },
          { 27544U, 32768U, 0U } }
    };
    static const uint16_t compound_backward_reference[3][2][3] = {
        { { 2235U, 32768U, 0U }, { 1423U, 32768U, 0U } },
        { { 17182U, 32768U, 0U }, { 15175U, 32768U, 0U } },
        { { 30606U, 32768U, 0U }, { 30489U, 32768U, 0U } }
    };
    static const uint16_t tx_type_set1[2][17] = {
        { 4458U, 5560U, 7695U, 9709U, 13330U, 14789U, 17537U,
          20266U, 21504U, 22848U, 23934U, 25474U, 27727U, 28915U,
          30631U, 32768U, 0U },
        { 1645U, 2573U, 4778U, 5711U, 7807U, 8622U, 10522U,
          15357U, 17674U, 20408U, 22517U, 25010U, 27116U, 28856U,
          30749U, 32768U, 0U }
    };
    static const uint16_t tx_type_set2[13] = {
        770U, 2421U, 5225U, 12907U, 15819U, 18927U, 21561U,
        24089U, 26595U, 28526U, 30529U, 32768U, 0U
    };
    static const uint16_t tx_type_set3[4][3] = {
        { 16384U, 32768U, 0U }, { 4167U, 32768U, 0U },
        { 1998U, 32768U, 0U }, { 748U, 32768U, 0U }
    };
    static const uint16_t new_mv[6][3] = {
        { 24035U, 32768U, 0U }, { 16630U, 32768U, 0U },
        { 15339U, 32768U, 0U }, { 8386U, 32768U, 0U },
        { 12222U, 32768U, 0U }, { 4676U, 32768U, 0U }
    };
    static const uint16_t zero_mv[2][3] = {
        { 2175U, 32768U, 0U }, { 1054U, 32768U, 0U }
    };
    static const uint16_t reference_mv[6][3] = {
        { 23974U, 32768U, 0U }, { 24188U, 32768U, 0U },
        { 17848U, 32768U, 0U }, { 28622U, 32768U, 0U },
        { 24312U, 32768U, 0U }, { 19923U, 32768U, 0U }
    };
    static const uint16_t drl_mode[3][3] = {
        { 13104U, 32768U, 0U }, { 24560U, 32768U, 0U },
        { 18945U, 32768U, 0U }
    };
    static const uint16_t compound_mode[8][9] = {
        { 7760U, 13823U, 15808U, 17641U, 19156U, 20666U, 26891U, 32768U, 0U },
        { 10730U, 19452U, 21145U, 22749U, 24039U, 25131U, 28724U, 32768U, 0U },
        { 10664U, 20221U, 21588U, 22906U, 24295U, 25387U, 28436U, 32768U, 0U },
        { 13298U, 16984U, 20471U, 24182U, 25067U, 25736U, 26422U, 32768U, 0U },
        { 18904U, 23325U, 25242U, 27432U, 27898U, 28258U, 30758U, 32768U, 0U },
        { 10725U, 17454U, 20124U, 22820U, 24195U, 25168U, 26046U, 32768U, 0U },
        { 17125U, 24273U, 25814U, 27492U, 28214U, 28704U, 30592U, 32768U, 0U },
        { 13046U, 23214U, 24505U, 25942U, 27435U, 28442U, 29330U, 32768U, 0U }
    };
    static const uint16_t switchable_interp[16][4] = {
        { 31935U, 32720U, 32768U, 0U },
        { 5568U, 32719U, 32768U, 0U },
        { 422U, 2938U, 32768U, 0U },
        { 28244U, 32608U, 32768U, 0U },
        { 31206U, 31953U, 32768U, 0U },
        { 4862U, 32121U, 32768U, 0U },
        { 770U, 1152U, 32768U, 0U },
        { 20889U, 25637U, 32768U, 0U },
        { 31910U, 32724U, 32768U, 0U },
        { 4120U, 32712U, 32768U, 0U },
        { 305U, 2247U, 32768U, 0U },
        { 27403U, 32636U, 32768U, 0U },
        { 31022U, 32009U, 32768U, 0U },
        { 2963U, 32093U, 32768U, 0U },
        { 601U, 943U, 32768U, 0U },
        { 14969U, 21398U, 32768U, 0U }
    };
    static const uint16_t motion_mode[22][4] = {
        { 10923U, 21845U, 32768U, 0U },
        { 10923U, 21845U, 32768U, 0U },
        { 10923U, 21845U, 32768U, 0U },
        { 7651U, 24760U, 32768U, 0U },
        { 4738U, 24765U, 32768U, 0U },
        { 5391U, 25528U, 32768U, 0U },
        { 19419U, 26810U, 32768U, 0U },
        { 5123U, 23606U, 32768U, 0U },
        { 11606U, 24308U, 32768U, 0U },
        { 26260U, 29116U, 32768U, 0U },
        { 20360U, 28062U, 32768U, 0U },
        { 21679U, 26830U, 32768U, 0U },
        { 29516U, 30701U, 32768U, 0U },
        { 28898U, 30397U, 32768U, 0U },
        { 30878U, 31335U, 32768U, 0U },
        { 32507U, 32558U, 32768U, 0U },
        { 10923U, 21845U, 32768U, 0U },
        { 10923U, 21845U, 32768U, 0U },
        { 28799U, 31390U, 32768U, 0U },
        { 26431U, 30774U, 32768U, 0U },
        { 28973U, 31594U, 32768U, 0U },
        { 29742U, 31203U, 32768U, 0U }
    };
    static const uint16_t obmc[22][3] = {
        { 16384U, 32768U, 0U }, { 16384U, 32768U, 0U },
        { 16384U, 32768U, 0U }, { 10437U, 32768U, 0U },
        { 9371U, 32768U, 0U }, { 9301U, 32768U, 0U },
        { 17432U, 32768U, 0U }, { 14423U, 32768U, 0U },
        { 15142U, 32768U, 0U }, { 25817U, 32768U, 0U },
        { 22823U, 32768U, 0U }, { 22083U, 32768U, 0U },
        { 30128U, 32768U, 0U }, { 31014U, 32768U, 0U },
        { 31560U, 32768U, 0U }, { 32638U, 32768U, 0U },
        { 16384U, 32768U, 0U }, { 16384U, 32768U, 0U },
        { 23664U, 32768U, 0U }, { 20901U, 32768U, 0U },
        { 24008U, 32768U, 0U }, { 26879U, 32768U, 0U }
    };
    static const uint16_t interintra[4][3] = {
        { 16384U, 32768U, 0U }, { 26887U, 32768U, 0U },
        { 27597U, 32768U, 0U }, { 30237U, 32768U, 0U }
    };
    static const uint16_t interintra_mode[4][5] = {
        { 8192U, 16384U, 24576U, 32768U, 0U },
        { 1875U, 11082U, 27332U, 32768U, 0U },
        { 2473U, 9996U, 26388U, 32768U, 0U },
        { 4238U, 11537U, 25926U, 32768U, 0U }
    };
    static const uint16_t wedge_interintra_values[22] = {
        16384U, 16384U, 16384U, 20036U, 24957U, 26704U,
        27530U, 29564U, 29444U, 26872U, 16384U, 16384U,
        16384U, 16384U, 16384U, 16384U, 16384U, 16384U,
        16384U, 16384U, 16384U, 16384U
    };
    static const uint16_t compound_type_values[22] = {
        16384U, 16384U, 16384U, 23431U, 13171U, 11470U,
        9770U, 9100U, 8233U, 6172U, 16384U, 16384U,
        16384U, 16384U, 16384U, 16384U, 16384U, 16384U,
        11820U, 7701U, 16384U, 16384U
    };
    static const uint16_t uniform_wedge[17] = {
        2048U, 4096U, 6144U, 8192U, 10240U, 12288U, 14336U,
        16384U, 18432U, 20480U, 22528U, 24576U, 26624U,
        28672U, 30720U, 32768U, 0U
    };
    static const uint16_t wedge_index_values[9][17] = {
        { 2438U, 4440U, 6599U, 8663U, 11005U, 12874U, 15751U, 18094U,
          20359U, 22362U, 24127U, 25702U, 27752U, 29450U, 31171U,
          32768U, 0U },
        { 806U, 3266U, 6005U, 6738U, 7218U, 7367U, 7771U, 14588U,
          16323U, 17367U, 18452U, 19422U, 22839U, 26127U, 29629U,
          32768U, 0U },
        { 2779U, 3738U, 4683U, 7213U, 7775U, 8017U, 8655U, 14357U,
          17939U, 21332U, 24520U, 27470U, 29456U, 30529U, 31656U,
          32768U, 0U },
        { 1684U, 3625U, 5675U, 7108U, 9302U, 11274U, 14429U, 17144U,
          19163U, 20961U, 22884U, 24471U, 26719U, 28714U, 30877U,
          32768U, 0U },
        { 1142U, 3491U, 6277U, 7314U, 8089U, 8355U, 9023U, 13624U,
          15369U, 16730U, 18114U, 19313U, 22521U, 26012U, 29550U,
          32768U, 0U },
        { 2742U, 4195U, 5727U, 8035U, 8980U, 9336U, 10146U, 14124U,
          17270U, 20533U, 23434U, 25972U, 27944U, 29570U, 31416U,
          32768U, 0U },
        { 1727U, 3948U, 6101U, 7796U, 9841U, 12344U, 15766U, 18944U,
          20638U, 22038U, 23963U, 25311U, 26988U, 28766U, 31012U,
          32768U, 0U },
        { 154U, 987U, 1925U, 2051U, 2088U, 2111U, 2151U, 23033U,
          23703U, 24284U, 24985U, 25684U, 27259U, 28883U, 30911U,
          32768U, 0U },
        { 1135U, 1322U, 1493U, 2635U, 2696U, 2737U, 2770U, 21016U,
          22935U, 25057U, 27251U, 29173U, 30089U, 30960U, 31933U,
          32768U, 0U }
    };
    static const uint8_t wedge_block_index[9] = {
        3U, 4U, 5U, 6U, 7U, 8U, 9U, 18U, 19U
    };
    static const uint16_t compound_index[6][3] = {
        { 18244U, 32768U, 0U }, { 12865U, 32768U, 0U },
        { 7053U, 32768U, 0U }, { 13259U, 32768U, 0U },
        { 9334U, 32768U, 0U }, { 4644U, 32768U, 0U }
    };
    static const uint16_t compound_group_index[6][3] = {
        { 26607U, 32768U, 0U }, { 22891U, 32768U, 0U },
        { 18840U, 32768U, 0U }, { 24594U, 32768U, 0U },
        { 19934U, 32768U, 0U }, { 22674U, 32768U, 0U }
    };
    static const uint16_t mv_joint[5] = {
        4096U, 11264U, 19328U, 32768U, 0U
    };
    static const uint16_t mv_class[12] = {
        28672U, 30976U, 31858U, 32320U, 32551U, 32656U,
        32740U, 32757U, 32762U, 32767U, 32768U, 0U
    };
    static const uint16_t class0_fr[2][5] = {
        { 16384U, 24576U, 26624U, 32768U, 0U },
        { 12288U, 21248U, 24128U, 32768U, 0U }
    };
    static const uint16_t fr[5] = {
        8192U, 17408U, 21248U, 32768U, 0U
    };
    static const uint16_t binary[3] = { 16384U, 32768U, 0U };
    static const uint16_t class0_hp[3] = { 20480U, 32768U, 0U };
    static const uint16_t class0_bit[3] = { 27648U, 32768U, 0U };
    static const uint16_t bits[10][3] = {
        { 17408U, 32768U, 0U }, { 17920U, 32768U, 0U },
        { 18944U, 32768U, 0U }, { 20480U, 32768U, 0U },
        { 22528U, 32768U, 0U }, { 24576U, 32768U, 0U },
        { 28672U, 32768U, 0U }, { 29952U, 32768U, 0U },
        { 29952U, 32768U, 0U }, { 30720U, 32768U, 0U }
    };
    unsigned int component;
    unsigned int block;

    if (cdfs == 0) return;
    avifdec_memory_copy(cdfs->is_inter, is_inter, sizeof(is_inter));
    avifdec_memory_copy(cdfs->skip_mode, skip_mode, sizeof(skip_mode));
    avifdec_memory_copy(cdfs->compound_reference, compound_reference,
                        sizeof(compound_reference));
    avifdec_memory_copy(cdfs->compound_reference_type,
                        compound_reference_type,
                        sizeof(compound_reference_type));
    avifdec_memory_copy(cdfs->single_reference, single_reference,
                        sizeof(single_reference));
    avifdec_memory_copy(cdfs->unidirectional_reference,
                        unidirectional_reference,
                        sizeof(unidirectional_reference));
    avifdec_memory_copy(cdfs->compound_forward_reference,
                        compound_forward_reference,
                        sizeof(compound_forward_reference));
    avifdec_memory_copy(cdfs->compound_backward_reference,
                        compound_backward_reference,
                        sizeof(compound_backward_reference));
    avifdec_memory_copy(cdfs->tx_type_set1, tx_type_set1,
                        sizeof(tx_type_set1));
    avifdec_memory_copy(cdfs->tx_type_set2, tx_type_set2,
                        sizeof(tx_type_set2));
    avifdec_memory_copy(cdfs->tx_type_set3, tx_type_set3,
                        sizeof(tx_type_set3));
    avifdec_memory_copy(cdfs->new_mv, new_mv, sizeof(new_mv));
    avifdec_memory_copy(cdfs->zero_mv, zero_mv, sizeof(zero_mv));
    avifdec_memory_copy(cdfs->reference_mv, reference_mv,
                        sizeof(reference_mv));
    avifdec_memory_copy(cdfs->drl_mode, drl_mode, sizeof(drl_mode));
    avifdec_memory_copy(cdfs->compound_mode, compound_mode,
                        sizeof(compound_mode));
    avifdec_memory_copy(cdfs->switchable_interp, switchable_interp,
                        sizeof(switchable_interp));
    avifdec_memory_copy(cdfs->motion_mode, motion_mode,
                        sizeof(motion_mode));
    avifdec_memory_copy(cdfs->obmc, obmc, sizeof(obmc));
    avifdec_memory_copy(cdfs->interintra, interintra,
                        sizeof(interintra));
    avifdec_memory_copy(cdfs->interintra_mode, interintra_mode,
                        sizeof(interintra_mode));
    avifdec_memory_copy(cdfs->compound_index, compound_index,
                        sizeof(compound_index));
    avifdec_memory_copy(cdfs->compound_group_index, compound_group_index,
                        sizeof(compound_group_index));
    for (block = 0U; block < 22U; ++block) {
        cdfs->wedge_interintra[block][0] =
            wedge_interintra_values[block];
        cdfs->wedge_interintra[block][1] = 32768U;
        cdfs->wedge_interintra[block][2] = 0U;
        cdfs->compound_type[block][0] = compound_type_values[block];
        cdfs->compound_type[block][1] = 32768U;
        cdfs->compound_type[block][2] = 0U;
        avifdec_memory_copy(cdfs->wedge_index[block], uniform_wedge,
                            sizeof(uniform_wedge));
    }
    for (block = 0U; block < 9U; ++block) {
        avifdec_memory_copy(cdfs->wedge_index[wedge_block_index[block]],
                            wedge_index_values[block],
                            sizeof(wedge_index_values[block]));
    }
    avifdec_memory_copy(cdfs->mv_joint, mv_joint, sizeof(mv_joint));
    for (component = 0U; component < 2U; ++component) {
        Av1MvComponentCdfs *mv = &cdfs->mv_component[component];

        avifdec_memory_copy(mv->class_cdf, mv_class, sizeof(mv_class));
        avifdec_memory_copy(mv->class0_fr, class0_fr, sizeof(class0_fr));
        avifdec_memory_copy(mv->fr, fr, sizeof(fr));
        avifdec_memory_copy(mv->sign, binary, sizeof(binary));
        avifdec_memory_copy(mv->class0_hp, class0_hp, sizeof(class0_hp));
        avifdec_memory_copy(mv->hp, binary, sizeof(binary));
        avifdec_memory_copy(mv->class0_bit, class0_bit, sizeof(class0_bit));
        avifdec_memory_copy(mv->bits, bits, sizeof(bits));
    }
}

static AvifdecStatus av1_mv_read_component(
    Av1SymbolDecoder *decoder,
    Av1MvComponentCdfs *cdfs,
    int force_integer_mv,
    int allow_high_precision_mv,
    int32_t *component) {
    uint32_t sign;
    uint32_t mv_class;
    uint32_t magnitude;

    sign = av1_symbol_read(decoder, cdfs->sign, 2U);
    mv_class = av1_symbol_read(decoder, cdfs->class_cdf, 11U);
    if (decoder->status != AVIFDEC_OK) return decoder->status;
    if (mv_class == 0U) {
        uint32_t class0_bit =
            av1_symbol_read(decoder, cdfs->class0_bit, 2U);
        uint32_t fraction = force_integer_mv
                            ? 3U : av1_symbol_read(
                                decoder, cdfs->class0_fr[class0_bit], 4U);
        uint32_t high_precision = allow_high_precision_mv
                                  ? av1_symbol_read(
                                      decoder, cdfs->class0_hp, 2U)
                                  : 1U;

        magnitude = ((class0_bit << 3) | (fraction << 1) |
                     high_precision) + 1U;
    } else {
        uint32_t offset = 0U;
        uint32_t fraction;
        uint32_t high_precision;
        unsigned int bit;

        for (bit = 0U; bit < mv_class; ++bit) {
            offset |= av1_symbol_read(decoder, cdfs->bits[bit], 2U) << bit;
        }
        fraction = force_integer_mv
                   ? 3U : av1_symbol_read(decoder, cdfs->fr, 4U);
        high_precision = allow_high_precision_mv
                         ? av1_symbol_read(decoder, cdfs->hp, 2U) : 1U;
        magnitude = (2U << (mv_class + 2U)) +
                    ((offset << 3) | (fraction << 1) | high_precision) + 1U;
    }
    if (decoder->status != AVIFDEC_OK) return decoder->status;
    *component = sign ? -(int32_t)magnitude : (int32_t)magnitude;
    return AVIFDEC_OK;
}

AvifdecStatus av1_mv_read(Av1SymbolDecoder *decoder,
                          Av1InterCdfs *cdfs,
                          int force_integer_mv,
                          int allow_high_precision_mv,
                          Av1MotionVector prediction,
                          Av1MotionVector *mv) {
    if (cdfs == 0) return AVIFDEC_INVALID_ARGUMENT;
    return av1_mv_read_cdfs(
        decoder, cdfs->mv_joint, cdfs->mv_component,
        force_integer_mv, allow_high_precision_mv, prediction, mv);
}

AvifdecStatus av1_mv_read_cdfs(Av1SymbolDecoder *decoder,
                               uint16_t joint_cdf[5],
                               Av1MvComponentCdfs components[2],
                               int force_integer_mv,
                               int allow_high_precision_mv,
                               Av1MotionVector prediction,
                               Av1MotionVector *mv) {
    uint32_t joint;
    int32_t row = 0;
    int32_t column = 0;
    AvifdecStatus status;

    if (decoder == 0 || joint_cdf == 0 || components == 0 || mv == 0 ||
        (force_integer_mv != 0 && force_integer_mv != 1) ||
        (allow_high_precision_mv != 0 && allow_high_precision_mv != 1)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    joint = av1_symbol_read(decoder, joint_cdf, 4U);
    if (joint == 2U || joint == 3U) {
        status = av1_mv_read_component(
            decoder, &components[0], force_integer_mv,
            allow_high_precision_mv, &row);
        if (status != AVIFDEC_OK) return status;
    }
    if (joint == 1U || joint == 3U) {
        status = av1_mv_read_component(
            decoder, &components[1], force_integer_mv,
            allow_high_precision_mv, &column);
        if (status != AVIFDEC_OK) return status;
    }
    if (decoder->status != AVIFDEC_OK) return decoder->status;
    mv->row = prediction.row + row;
    mv->column = prediction.column + column;
    return AVIFDEC_OK;
}

static int32_t av1_mv_clip(int32_t minimum, int32_t maximum, int32_t value) {
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static int32_t av1_mv_round_signed(int64_t value, unsigned int bits) {
    int64_t rounding = (int64_t)1 << (bits - 1U);

    if (value < 0) return -(int32_t)((-value + rounding) >> bits);
    return (int32_t)((value + rounding) >> bits);
}

AvifdecStatus av1_mv_project(Av1MotionVector mv,
                             int32_t numerator,
                             uint32_t denominator,
                             Av1MotionVector *projected) {
    static const uint16_t division_multiplier[32] = {
        0U, 16384U, 8192U, 5461U, 4096U, 3276U, 2730U, 2340U,
        2048U, 1820U, 1638U, 1489U, 1365U, 1260U, 1170U, 1092U,
        1024U, 963U, 910U, 862U, 819U, 780U, 744U, 712U,
        682U, 655U, 630U, 606U, 585U, 564U, 546U, 528U
    };
    int32_t clipped_numerator;
    uint32_t clipped_denominator;
    int32_t row;
    int32_t column;

    if (projected == 0 || denominator == 0U) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    clipped_denominator = denominator > 31U ? 31U : denominator;
    clipped_numerator = av1_mv_clip(-31, 31, numerator);
    row = av1_mv_round_signed(
        (int64_t)mv.row * clipped_numerator *
            division_multiplier[clipped_denominator], 14U);
    column = av1_mv_round_signed(
        (int64_t)mv.column * clipped_numerator *
            division_multiplier[clipped_denominator], 14U);
    projected->row = av1_mv_clip(-16383, 16383, row);
    projected->column = av1_mv_clip(-16383, 16383, column);
    return AVIFDEC_OK;
}

AvifdecStatus av1_mv_global(uint8_t type,
                            const int32_t params[6],
                            uint32_t mi_row,
                            uint32_t mi_column,
                            uint32_t block_width,
                            uint32_t block_height,
                            int force_integer_mv,
                            int allow_high_precision_mv,
                            Av1MotionVector *mv) {
    const unsigned int warped_precision_bits = 16U;

    if (params == 0 || mv == 0 || type > 3U || block_width == 0U ||
        block_height == 0U ||
        (force_integer_mv != 0 && force_integer_mv != 1) ||
        (allow_high_precision_mv != 0 && allow_high_precision_mv != 1)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    if (type == 0U) {
        mv->row = 0;
        mv->column = 0;
    } else if (type == 1U) {
        mv->row = params[0] >> (warped_precision_bits - 3U);
        mv->column = params[1] >> (warped_precision_bits - 3U);
    } else {
        int64_t x = (int64_t)mi_column * 4 + block_width / 2U - 1;
        int64_t y = (int64_t)mi_row * 4 + block_height / 2U - 1;
        int64_t centered_x =
            ((int64_t)params[2] - (1LL << warped_precision_bits)) * x +
            (int64_t)params[3] * y + params[0];
        int64_t centered_y =
            (int64_t)params[4] * x +
            ((int64_t)params[5] - (1LL << warped_precision_bits)) * y +
            params[1];
        unsigned int shift = warped_precision_bits -
                             (allow_high_precision_mv ? 3U : 2U);

        mv->row = av1_mv_round_signed(centered_y, shift);
        mv->column = av1_mv_round_signed(centered_x, shift);
        if (!allow_high_precision_mv) {
            mv->row *= 2;
            mv->column *= 2;
        }
    }
    av1_mv_lower_precision(mv, force_integer_mv, allow_high_precision_mv);
    return AVIFDEC_OK;
}

void av1_mv_lower_precision(Av1MotionVector *mv,
                            int force_integer_mv,
                            int allow_high_precision_mv) {
    int32_t *components[2];
    unsigned int index;

    if (mv == 0 || allow_high_precision_mv) return;
    components[0] = &mv->row;
    components[1] = &mv->column;
    for (index = 0U; index < 2U; ++index) {
        int32_t value = *components[index];

        if (force_integer_mv) {
            uint32_t magnitude = value < 0
                                 ? (uint32_t)(-(int64_t)value)
                                 : (uint32_t)value;
            int32_t rounded = (int32_t)(((magnitude + 3U) >> 3) << 3);

            *components[index] = value > 0 ? rounded : -rounded;
        } else if ((value & 1) != 0) {
            *components[index] += value > 0 ? -1 : 1;
        }
    }
}

void av1_mv_clamp(Av1MotionVector *mv,
                  uint32_t mi_row,
                  uint32_t mi_column,
                  uint32_t block_width4,
                  uint32_t block_height4,
                  uint32_t mi_rows,
                  uint32_t mi_columns,
                  uint32_t border) {
    int64_t top;
    int64_t bottom;
    int64_t left;
    int64_t right;
    uint64_t row_border;
    uint64_t column_border;

    if (mv == 0 || block_width4 == 0U || block_height4 == 0U ||
        mi_row > mi_rows || mi_column > mi_columns) {
        return;
    }
    row_border = (uint64_t)border + (uint64_t)block_height4 * 32U;
    column_border = (uint64_t)border + (uint64_t)block_width4 * 32U;
    top = -(int64_t)mi_row * 32 - (int64_t)row_border;
    bottom = ((int64_t)mi_rows - block_height4 - mi_row) * 32 +
             (int64_t)row_border;
    left = -(int64_t)mi_column * 32 - (int64_t)column_border;
    right = ((int64_t)mi_columns - block_width4 - mi_column) * 32 +
            (int64_t)column_border;
    if (top < INT32_MIN) top = INT32_MIN;
    if (bottom > INT32_MAX) bottom = INT32_MAX;
    if (left < INT32_MIN) left = INT32_MIN;
    if (right > INT32_MAX) right = INT32_MAX;
    mv->row = av1_mv_clip((int32_t)top, (int32_t)bottom, mv->row);
    mv->column = av1_mv_clip((int32_t)left, (int32_t)right, mv->column);
}

static int av1_mv_equal(Av1MotionVector first, Av1MotionVector second) {
    return first.row == second.row && first.column == second.column;
}

AvifdecStatus av1_mv_stack_add(Av1MvStack *stack,
                               Av1MotionVector first,
                               Av1MotionVector second,
                               int compound,
                               uint16_t weight) {
    unsigned int index;

    if (stack == 0 || (compound != 0 && compound != 1) ||
        stack->count > AV1_MAX_MV_STACK_SIZE) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    for (index = 0U; index < stack->count; ++index) {
        Av1MvCandidate *candidate = &stack->candidates[index];

        if (av1_mv_equal(candidate->mv[0], first) &&
            (!compound || av1_mv_equal(candidate->mv[1], second))) {
            uint32_t combined = (uint32_t)candidate->weight + weight;
            candidate->weight = combined > UINT16_MAX
                                ? UINT16_MAX : (uint16_t)combined;
            return AVIFDEC_OK;
        }
    }
    if (stack->count == AV1_MAX_MV_STACK_SIZE) {
        return AVIFDEC_OK;
    }
    stack->candidates[stack->count].mv[0] = first;
    stack->candidates[stack->count].mv[1] = second;
    stack->candidates[stack->count].weight = weight;
    ++stack->count;
    return AVIFDEC_OK;
}

void av1_mv_stack_sort(Av1MvStack *stack, uint8_t start) {
    unsigned int index;

    if (stack == 0 || start >= stack->count) return;
    for (index = start + 1U; index < stack->count; ++index) {
        Av1MvCandidate candidate = stack->candidates[index];
        unsigned int position = index;

        while (position > start &&
               candidate.weight > stack->candidates[position - 1U].weight) {
            stack->candidates[position] = stack->candidates[position - 1U];
            --position;
        }
        stack->candidates[position] = candidate;
    }
}