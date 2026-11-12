// The modes added after FM, seen together: tuning table, test signals, status lines.
#include "dect2/source/source.h"
#include <map>
#include <mutex>
#include "dect2/engine/mode_tuning.h"
#include "dect2/engine/mode_registry.h"
#include "dect2/dvbs/dvbs_rx.h"
#include "dect2/dvbs/dvbs_gen.h"
#include "dect2/dtmb/dtmb_rx.h"
#include "dect2/dtmb/dtmb_gen.h"
#include "dect2/atv/atv_rx.h"
#include "dect2/atv/atv_gen.h"
#include "dect2/dmr/dmr_rx.h"
#include "dect2/dmr/dmr_gen.h"
#include "dect2/drm/drm_rx.h"
#include "dect2/drm/drm_gen.h"
#include "dect2/adsb/adsb_rx.h"
#include "dect2/adsb/adsb_gen.h"
#include "dect2/atsc3/atsc3_synth.h"
#include "dect2/atsc/atsc_gen.h"
#include "dect2/dvbt/dvbt_gen.h"
#include "dect2/dab/dab_gen.h"
#include "dect2/gnss/gnss_rx.h"
#include "dect2/gnss/gnss_gen.h"
#include "dect2/sonde/sonde_rx.h"
#include "dect2/sonde/sonde_gen.h"
#include "dect2/ais/ais_rx.h"
#include "dect2/ais/ais_gen.h"
#include "dect2/marine/marine_rx.h"
#include "dect2/marine/marine_gen.h"
#include "dect2/acars/acars_rx.h"
#include "dect2/acars/acars_gen.h"
#include "dect2/inmc/inmc_rx.h"
#include "dect2/inmc/inmc_gen.h"
#include "dect2/aero/aero_rx.h"
#include "dect2/aero/aero_gen.h"
#include "dect2/iridium/iridium_rx.h"
#include "dect2/iridium/iridium_gen.h"
#include "dect2/mesh/mesh_rx.h"
#include "dect2/mesh/mesh_gen.h"
#include "dect2/hdr/hdr_rx.h"
#include "dect2/hdr/hdr_gen.h"
#include "dect2/cdr/cdr_rx.h"
#include "dect2/cdr/cdr_gen.h"
#include "dect2/pager/pager_rx.h"
#include "dect2/pager/pager_gen.h"
#include "dect2/packet/packet_rx.h"
#include "dect2/packet/packet_gen.h"
#include "dect2/hfdig/hfdig_rx.h"
#include "dect2/hfdig/hfdig_gen.h"
#include "dect2/airband/airband_rx.h"
#include "dect2/airband/airband_gen.h"
#include "dect2/vdl2/vdl2_rx.h"
#include "dect2/vdl2/vdl2_gen.h"
#include "dect2/vdl4/vdl4_rx.h"
#include "dect2/vdl4/vdl4_gen.h"
#include "dect2/lbj/lbj_gen.h"
#include "dect2/ism/ism_rx.h"
#include "dect2/ism/ism_gen.h"
#include "dect2/vor/vor_rx.h"
#include "dect2/vor/vor_gen.h"
#include "dect2/m17/m17_rx.h"
#include "dect2/dstar/dstar_rx.h"
#include "dect2/dstar/dstar_gen.h"
#include "dect2/nxdn/nxdn_rx.h"
#include "dect2/nxdn/nxdn_gen.h"
#include "dect2/dpmr/dpmr_rx.h"
#include "dect2/dpmr/dpmr_gen.h"
#include "dect2/tetra/tetra_rx.h"
#include "dect2/tetra/tetra_gen.h"
#include "dect2/tetrapol/tetrapol_rx.h"
#include "dect2/tetrapol/tetrapol_gen.h"
#include "dect2/tetrapol/tetrapol_tel.h"
#include "dect2/elektro/elektro_rx.h"
#include "dect2/elektro/elektro_gen.h"
#include "dect2/elektro/elektro_tel.h"
#include "dect2/gk2a/gk2a_rx.h"
#include "dect2/gk2a/gk2a_gen.h"
#include "dect2/gk2a/gk2a_tel.h"
#include "dect2/uat/uat_rx.h"
#include "dect2/uat/uat_gen.h"
#include "dect2/uat/uat_tel.h"
#include "dect2/mac/mac_rx.h"
#include "dect2/mac/mac_gen.h"
#include "dect2/psk/psk_rx.h"
#include "dect2/psk/psk_gen.h"
#include "dect2/js8/js8_rx.h"
#include "dect2/js8/js8_gen.h"
#include "dect2/ale/ale_rx.h"
#include "dect2/ale/ale_gen.h"
#include "dect2/specx/specx_rx.h"
#include "dect2/specx/specx_tel.h"
#include "dect2/specx/specx_gen.h"
#include "dect2/specan/specan_rx.h"
#include "dect2/specan/specan_gen.h"
#include "dect2/astro/astro_rx.h"
#include "dect2/astro/astro_tel.h"
#include "dect2/astro/astro_gen.h"
#include "dect2/m17/m17_gen.h"
#include "dect2/hfdl/hfdl_rx.h"
#include "dect2/hfdl/hfdl_gen.h"
#include "dect2/cell/cell_rx.h"
#include "dect2/cell/cell_gen.h"
#include "dect2/cell/cell_tel.h"
#include "dect2/cw/cw_rx.h"
#include "dect2/cw/cw_gen.h"
#include "dect2/gsmr/gsmr_rx.h"
#include "dect2/gsmr/gsmr_gen.h"
#include "dect2/gsmr/gsmr_tel.h"
#include "dect2/dvbc/dvbc_rx.h"
#include "dect2/dvbc2/dvbc2_rx.h"
#include "dect2/dvbc2/dvbc2_gen.h"
#include "dect2/orbcomm/orbcomm_rx.h"
#include "dect2/uplink/uplink_rx.h"
#include "dect2/uplink/uplink_gen.h"
#include "dect2/p25/p25_rx.h"
#include "dect2/p25/p25_gen.h"
#include "dect2/ble/ble_rx.h"
#include "dect2/ble/ble_gen.h"
#include "dect2/btc/btc_rx.h"
#include "dect2/btc/btc_gen.h"
#include "dect2/btc/btc_tel.h"
#include "dect2/iono/iono_rx.h"
#include "dect2/iono/iono_tel.h"
#include "dect2/iono/iono_gen.h"
#include "dect2/qo100/qo100_rx.h"
#include "dect2/qo100/qo100_tel.h"
#include "dect2/qo100/qo100_gen.h"
#include "dect2/wifi/wifi_rx.h"
#include "dect2/wifi/wifi_tel.h"
#include "dect2/wifi/wifi_gen.h"
#include "dect2/general/general_rx.h"
#include "dect2/general/general_tel.h"
#include "dect2/general/general_gen.h"
#include "dect2/time/time_rx.h"
#include "dect2/time/time_tel.h"
#include "dect2/time/time_gen.h"
#include "dect2/cubesat/cubesat_rx.h"
#include "dect2/cubesat/cubesat_tel.h"
#include "dect2/cubesat/cubesat_gen.h"
#include "dect2/globalstar/globalstar_rx.h"
#include "dect2/globalstar/globalstar_tel.h"
#include "dect2/globalstar/globalstar_gen.h"
#include "dect2/vdes/vdes_rx.h"
#include "dect2/vdes/vdes_tel.h"
#include "dect2/vdes/vdes_gen.h"
#include "dect2/zigbee/zigbee_rx.h"
#include "dect2/zigbee/zigbee_tel.h"
#include "dect2/zigbee/zigbee_gen.h"
#include "dect2/flarm/flarm_rx.h"
#include "dect2/flarm/flarm_tel.h"
#include "dect2/flarm/flarm_gen.h"
#include "dect2/adsl/adsl_rx.h"
#include "dect2/avroad/avroad_rx.h"
#include "dect2/adsl/adsl_tel.h"
#include "dect2/adsl/adsl_gen.h"
#include "dect2/hfbeacon/hfbeacon_rx.h"
#include "dect2/hfbeacon/hfbeacon_tel.h"
#include "dect2/hfbeacon/hfbeacon_gen.h"
#include "dect2/ndb/ndb_rx.h"
#include "dect2/ndb/ndb_tel.h"
#include "dect2/ndb/ndb_gen.h"
#include "dect2/dme/dme_live.h"
#include "dect2/sitor/sitor_live.h"
#include "dect2/sitor/sitor_tel.h"
#include "dect2/dme/dme_tel.h"
#include "dect2/lbj/lbj_rx.h"
#include "dect2/railway/railway_rx.h"
#include "dect2/railway/railway_gen.h"
#include "dect2/horus/horus_rx.h"
#include "dect2/horus/horus_gen.h"
#include "dect2/lbj/lbj_tel.h"
#include "dect2/dgps/dgps_engine.h"
#include "dect2/eloran/eloran_engine.h"
#include "dect2/nfc/nfc_engine.h"
#include "dect2/nfc/nfc_gen.h"
#include "dect2/dji/dji_rx.h"
#include "dect2/dji/dji_tel.h"
#include "dect2/dji/dji_gen.h"
#include "dect2/nbiot/nbiot_rx.h"
#include "dect2/nbiot/nbiot_tel.h"
#include "dect2/nbiot/nbiot_gen.h"
#include "dect2/halow/halow_rx.h"
#include "dect2/halow/halow_tel.h"
#include "dect2/halow/halow_gen.h"
#include "dect2/codar/codar_rx.h"
#include "dect2/codar/codar_tel.h"
#include "dect2/codar/codar_gen.h"
#include "dect2/wb/wb_rx.h"
#include "dect2/wb/wb_tel.h"
#include "dect2/datawell/datawell_rx.h"
#include "dect2/datawell/datawell_tel.h"
#include "dect2/datawell/datawell_gen.h"
#include "dect2/same/same_rx.h"
#include "dect2/same/same_tel.h"
#include "dect2/same/same_gen.h"
#include "dect2/ant/ant_rx.h"
#include "dect2/ant/ant_tel.h"
#include "dect2/ant/ant_gen.h"
#include "dect2/dect/dect_rx.h"
#include "dect2/dect/dect_tel.h"
#include "dect2/dect/dect_gen.h"
#include "dect2/multitone/multitone_rx.h"
#include "dect2/multitone/multitone_tel.h"
#include "dect2/multitone/multitone_gen.h"
#include "dect2/loraraw/loraraw_rx.h"
#include "dect2/loraraw/loraraw_tel.h"
#include "dect2/loraraw/loraraw_gen.h"
#include "dect2/lorawan/lorawan_gen.h"
#include "dect2/lorawan/lorawan_rx.h"
#include "dect2/lorawan/lorawan_tel.h"
#include "dect2/lorasat/lorasat_rx.h"
#include "dect2/lorasat/lorasat_tel.h"
#include "dect2/lorasat/lorasat_gen.h"
#include "dect2/hfutil/hfutil_rx.h"
#include "dect2/hfutil/hfutil_tel.h"
#include "dect2/pactor/pactor_rx.h"
#include "dect2/pactor/pactor_gen.h"
#include "dect2/pactor/pactor_tel.h"
#include "dect2/pactorhf/pactorhf_rx.h"
#include "dect2/pactorhf/pactorhf_gen.h"
#include "dect2/pactorhf/pactorhf_tel.h"
#include "dect2/roshf/roshf_rx.h"
#include "dect2/roshf/roshf_gen.h"
#include "dect2/roshf/roshf_tel.h"
#include "dect2/ssdv/ssdv_rx.h"
#include "dect2/ssdv/ssdv_gen.h"
#include "dect2/ssdv/ssdv_tel.h"
#include "dect2/ebicab/ebicab_rx.h"
#include "dect2/ebicab/ebicab_gen.h"
#include "dect2/ebicab/ebicab_tel.h"
#include "dect2/tv/iptv_mode.h"
#include "dect2/inmsat/inmsat_rx.h"
#include "dect2/inmsat/inmsat_gen.h"
#include "dect2/inmsat/inmsat_tel.h"
#include "dect2/rclink/rc_rx.h"
#include "dect2/xlink/xlink_rx.h"
#include "dect2/cishf/cishf_rx.h"
#include "dect2/cishf/cishf_tel.h"
#include "dect2/w1a/w1a_rx.h"
#include "dect2/w1a/w1a_tel.h"
#include "dect2/nrf24/nrf24_rx.h"
#include "dect2/shortwave/shortwave_rx.h"
#include "dect2/shortwave/shortwave_gen.h"
#include "dect2/shortwave/shortwave_tel.h"
#include "dect2/fygeo/fygeo_rx.h"
#include "dect2/fygeo/fygeo_gen.h"
#include "dect2/fygeo/fygeo_tel.h"
#include "dect2/nrf24/nrf24_gen.h"
#include "dect2/nrf24/nrf24_tel.h"
#include "dect2/sensors/sensors_gen.h"
#include "dect2/sensors/sensors_rx.h"
#include "dect2/sensors/sensors_tel.h"
#include "dect2/bgan/bgan_gen.h"
#include "dect2/bgan/bgan_rx.h"
#include "dect2/sferics/sferics_gen.h"
#include "dect2/sferics/sferics_rx.h"
#include "dect2/sferics/ltpos_rx.h"
#include "dect2/sferics/ltpos_gen.h"
#include "dect2/fbr/fbr_rx.h"
#include "dect2/x40/x40_rx.h"
#include "dect2/fbr/fbr_gen.h"
#include "dect2/atis/atis_rx.h"
#include "dect2/selcal/selcal_rx.h"
#include "dect2/umts/umts_rx.h"
#include "dect2/cdma/cdma_rx.h"
#include "dect2/tdscdma/tdscdma_rx.h"
#include "dect2/cp16/cp16_gen.h"
#include "dect2/cp16/cp16_rx.h"
#include "dect2/vdl3/vdl3_gen.h"
#include "dect2/vdl3/vdl3_rx.h"
#include "dect2/selcal/selcal_tel.h"
#include "dect2/tones/tones_gen.h"
#include "dect2/atis/atis_gen.h"
#include "dect2/atis/atis_tel.h"
#include "dect2/wxsat/wxsat_rx.h"
#include "dect2/sd3/sd3_rx.h"
#include "dect2/sd3/sd3_gen.h"
#include "dect2/sd3/sd3_tel.h"
#include "dect2/wxsat/wxsat_gen.h"
#include "dect2/wxsat/wxsat_tel.h"
#include "dect2/hamsci/hamsci_rx.h"
#include "dect2/hamsci/hamsci_gen.h"
#include "dect2/xband/xband_rx.h"
#include "dect2/xband/xband_gen.h"
#include "dect2/xband/xband_tel.h"
#include "dect2/fy3/fy3_rx.h"
#include "dect2/fy3/fy3_gen.h"
#include "dect2/fy3/fy3_tel.h"
#include "dect2/esubghz/esubghz_rx.h"
#include "dect2/esubghz/esubghz_gen.h"
#include "dect2/esubghz/esubghz_tel.h"
#include "dect2/pskdsp/pskdsp_rx.h"
#include "dect2/pskdsp/pskdsp_tel.h"
#include "dect2/wisun/wisun_rx.h"
#include "dect2/wisun/wisun_tel.h"
#include "dect2/wisun/wisun_gen.h"
#include "dect2/zwave/zwave_rx.h"
#include "dect2/zwave/zwave_tel.h"
#include "dect2/zwave/zwave_gen.h"
#include "dect2/eczas/eczas_live.h"
#include "dect2/eczas/eczas_gen.h"
#include "dect2/eczas/eczas_tel.h"
#include "dect2/fpv/fpv_live.h"
#include "dect2/fpv/fpv_gen.h"
#include "dect2/fpv/fpv_tel.h"
#include "dect2/wmbus/wmbus_live.h"
#include "dect2/wmbus/wmbus_gen.h"
#include "dect2/wmbus/wmbus_tel.h"
#include "dect2/tones/tones_rx.h"
#include "dect2/tones/tones_gen.h"
#include "dect2/tones/tones_tel.h"
#include "dect2/sarsat/sarsat_rx.h"
#include "dect2/sarsat/sarsat_gen.h"
#include "dect2/sarsat/sarsat_tel.h"
#include "dect2/mt63/mt63_rx.h"
#include "dect2/mt63/mt63_gen.h"
#include "dect2/mt63/mt63_tel.h"
#include "dect2/ardop/ardop_rx.h"
#include "dect2/ardop/ardop_gen.h"
#include "dect2/ardop/ardop_tel.h"
#include "dect2/fsq/fsq_rx.h"
#include "dect2/fsq/fsq_gen.h"
#include "dect2/ifkp/ifkp_rx.h"
#include "dect2/kaal/kaal_live.h"
#include "dect2/kaal/kaal_tel.h"
#include "dect2/kait/kait_live.h"
#include "dect2/kait/kait_tel.h"
#include "dect2/kapp/kapp_live.h"
#include "dect2/kavb/kavb_live.h"
#include "dect2/kavb/kavb_tel.h"
#include "dect2/kaws/kaws_live.h"
#include "dect2/kaws/kaws_tel.h"
#include "dect2/kapp/kapp_tel.h"
#include "dect2/kaft/kaft_live.h"
#include "dect2/kaft/kaft_tel.h"
#include "dect2/ifkp/ifkp_gen.h"
#include "dect2/kaal/kaal_gen.h"
#include "dect2/kapp/kapp_gen.h"
#include "dect2/kaft/kaft_gen.h"
#include "dect2/kait/kait_gen.h"
#include "dect2/kavb/kavb_gen.h"
#include "dect2/kaws/kaws_gen.h"
#include "dect2/cquam/cquam_rx.h"
#include "dect2/cquam/cquam_gen.h"
#include "dect2/cquam/cquam_tel.h"
#include "dect2/mscatter/mscatter_live.h"
#include "dect2/mscatter/mscatter_gen.h"
#include "dect2/mscatter/mscatter_tel.h"
#include "dect2/goesdcs/goesdcs_live.h"
#include "dect2/goesdcs/goesdcs_gen.h"
#include "dect2/goesdcs/goesdcs_tel.h"
#include "dect2/goesdcs/hrdcp_live.h"
#include "dect2/goesdcs/hrdcp_gen.h"
#include "dect2/modeac/modeac_live.h"
#include "dect2/modeac/modeac_gen.h"
#include "dect2/modeac/modeac_tel.h"
#include "dect2/bletrk/bletrk_live.h"
#include "dect2/bletrk/bletrk_gen.h"
#include "dect2/bletrk/bletrk_tel.h"
#include "dect2/milhf/milhf_engine.h"
#include "dect2/sbas/sbas_engine.h"
#include "dect2/asterix/asterix_engine.h"
#include "dect2/argos/argos_rx.h"
#include "dect2/argos/argos_gen.h"
#include "dect2/argos/argos_tel.h"
#include "dect2/sigfox/sigfox_rx.h"
#include "dect2/sigfox/sigfox_gen.h"
#include "dect2/sigfox/sigfox_tel.h"
#include "dect2/satbeacon/satbeacon_live.h"
#include "dect2/satmisc/satdetect_live.h"
#include "dect2/satmisc/satdetect_tel.h"
#include "dect2/navic/navic.h"
#include "dect2/satbeacon/satbeacon_gen.h"
#include "dect2/satbeacon/satbeacon_tel.h"
#include "dect2/isdbs/isdbs_rx.h"
#include "dect2/dvbsh/dvbsh_engine.h"
#include "dect2/ltr/ltr_rx.h"
#include "dect2/ltr/ltr_tel.h"
#include "dect2/edacs/edacs_rx.h"
#include "dect2/edacs/edacs_tel.h"
#include "dect2/smartnet/smartnet_rx.h"
#include "dect2/smartnet/smartnet_tel.h"
#include "dect2/tetradmo/tetradmo_rx.h"
#include "dect2/tetradmo/tetradmo_tel.h"
#include "dect2/opensky/opensky_rx.h"
#include "dect2/opensky/opensky_tel.h"
#include "dect2/marinevhf/marinevhf_rx.h"
#include "dect2/marinevhf/marinevhf_tel.h"
#include "dect2/chanvoice/chv_rx.h"
#include "dect2/chanvoice/chv_tel.h"
#include "dect2/chanvoice/chv_gen.h"
#include "dect2/p25p2/p25p2_rx.h"
#include "dect2/p25p2/p25p2_tel.h"
#include "dect2/rid/rid_rx.h"
#include "dect2/rid/rid_tel.h"
#include "dect2/rid/rid_gen.h"
#include "dect2/meteor/meteor_rx.h"
#include "dect2/meteor/meteor_tel.h"
#include "dect2/meteor/meteor_gen.h"
#include "dect2/nr/nr_rx.h"
#include "dect2/nr/nr_tel.h"
#include "dect2/nr/nr_gen.h"
#include "dect2/goes/goes_rx.h"
#include "dect2/goes/goes_tel.h"
#include "dect2/goes/goes_gen.h"
#include "dect2/hrpt/hrpt_rx.h"
#include "dect2/hrpt/hrpt_tel.h"
#include "dect2/hrpt/hrpt_gen.h"
#include "dect2/engine/modes.h"
#include "dect2/tv/demo_ts.h"
#include "dect2/dsp/exact_resampler.h"
#include "dect2/fm/fm_gen.h"
#include "dect2/isdbt/isdbt_gen.h"
#include "dect2/isdbt/isdbt_rx.h"
#include <cmath>
#include <random>
#include <vector>

namespace dect2 {

namespace {
// The test signals of two of the older modes, so that every mode can be tried without a radio: FM (stereo with RDS) and ISDB-T (the test programme).
class FmSynth : public ModeSynth {
public:
    FmSynth(const SynthConfig& c, double rate) : rate_(rate) {
        FmGenConfig g;
        g.rate = rate; g.cnrDb = c.snrDb >= 40 ? 200 : c.snrDb + 10; g.cfoHz = c.cfoHz;
        g.ps = "ONAIR FM"; g.rt = "OnAir FM test signal: 1 kHz left, 3 kHz right";
        gen_ = std::make_unique<FmGenerator>(g);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        gen_->generate(out, n, 0.3f);
    }
private:
    double rate_;
    std::unique_ptr<FmGenerator> gen_;
};

// ATSC 1.0 (8-VSB): the test transport stream at the rate asked for (from the receiver's 6.5 Msps up to 2 samples a symbol, 21.5 Msps)
class AtscSynth : public ModeSynth {
public:
    AtscSynth(const SynthConfig& c, double rate) : rate_(rate) {
        atsc::ChannelConfig cc; cc.snrDb = c.snrDb; cc.cfoHz = c.cfoHz;
        gen_ = std::make_unique<atsc::Generator>(dvbt::testTsSource(), cc, rate);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        tmp_.clear();
        gen_->generate(n, tmp_);
        const float g = 0.22f / 6.7f;   // the 8-VSB signal has an rms of about 6.7 units (as the transmitter's AtscGen)
        for (size_t i = 0; i < n; i++) out[i] = tmp_[i] * g;
    }
private:
    double rate_;
    std::unique_ptr<atsc::Generator> gen_;
    std::vector<cf32> tmp_;
};

class IsdbtSynth : public ModeSynth {
public:
    // tsb: the ISDB-TSB test signal (three segments: the one-segment layer A and a two-segment layer B that carries the programme)
    IsdbtSynth(const SynthConfig& c, double rate, bool tsb = false) : rate_(rate), cfo_(c.cfoHz), sigma_((float)std::sqrt(std::pow(10.0, -c.snrDb / 10.0) / 2.0)), noise_(7) {
        isdbt::Params p;   // as isdbtgen: a one-segment layer A and a twelve-segment layer B that carries the programme
        p.mode = c.isdbtMode >= 1 && c.isdbtMode <= 3 ? c.isdbtMode : 3; p.guard = c.isdbtGuard >= 0 && c.isdbtGuard <= 3 ? c.isdbtGuard : (int)isdbt::kGi8; p.partial = true;
        p.layer[0].segments = 1; p.layer[0].mod = isdbt::kQpsk; p.layer[0].rate = isdbt::kR23; p.layer[0].ti = 2;
        p.tsb = tsb ? 3 : 0;
        p.layer[1].segments = tsb ? 2 : 12; p.layer[1].mod = c.isdbtMod >= 0 && c.isdbtMod <= 3 ? c.isdbtMod : (int)isdbt::k64Qam; p.layer[1].rate = c.isdbtRate >= 0 && c.isdbtRate <= 4 ? c.isdbtRate : (int)isdbt::kR34; p.layer[1].ti = 1;
        p.bwMhz = c.isdbtBwMhz == 7 || c.isdbtBwMhz == 8 ? c.isdbtBwMhz : 6;
        gen_ = std::make_unique<isdbt::Generator>(p, isdbt::singleLayerSource(1, demoTsSource(isdbt::layerBitrate(p, 1) * 0.9)), 1);
        const double fs = isdbt::sampleRateFor(p.bwMhz) / isdbt::tsbDecimation(p.tsb);
        poly_ = poly_resampler_.configure(fs, rate);   // 6 MHz at 10 Msps: 315/256, exact integer phases
        if (!poly_) rs_.configure(fs, rate);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        while (pending_.size() - pos_ < n) {
            gen_->nextFrame(frame_);
            if (poly_) poly_resampler_.process(frame_.data(), frame_.size(), pending_);
            else rs_.process(frame_.data(), frame_.size(), pending_);
        }
        const cf32* src = &pending_[pos_];
        if (cfo_ != 0) {   // carrier offset: an oscillator restarted from the exact phase every 128 samples
            const double dph = 2 * M_PI * cfo_ / rate_;
            const cf32 stp((float)std::cos(dph), (float)std::sin(dph));
            for (size_t i0 = 0; i0 < n; i0 += 128) {
                const double ph = std::remainder(dph * (double)(count_ + i0), 2 * M_PI);
                cf32 r((float)std::cos(ph), (float)std::sin(ph));
                const size_t i1 = std::min(n, i0 + 128);
                for (size_t i = i0; i < i1; i++) { out[i] = src[i] * r; r *= stp; }
            }
        } else for (size_t i = 0; i < n; i++) out[i] = src[i];
        count_ += n;
        if (sigma_ > 0) noise_.add(out, n, sigma_);
        for (size_t i = 0; i < n; i++) out[i] *= 0.25f;
        pos_ += n;
        if (pos_ > (1u << 20)) { pending_.erase(pending_.begin(), pending_.begin() + (long)pos_); pos_ = 0; }
    }
private:
    double rate_, cfo_;
    float sigma_;
    std::unique_ptr<isdbt::Generator> gen_;
    isdbt::PolyResampler poly_resampler_;
    bool poly_ = false;
    ExactResampler rs_;
    std::vector<cf32> frame_, pending_;
    size_t pos_ = 0;
    uint64_t count_ = 0;
    genutil::NoiseSource noise_;
};
}

// No mode limits where it can be tuned (SSTV from the ISS on 145.800, DMR on any band, ...): the radio's own range is the only limit
static std::vector<ModeTuning> anyFrequency(std::vector<ModeTuning> t) {
    for (auto& m : t) { m.minMhz = 0.001; m.maxMhz = kMaxTuneMhz; }
    return t;
}

// the satellite detector modes come from their own table (satdetect_systems.cpp)
static std::vector<ModeTuning> withSatDetect(std::vector<ModeTuning> t) {
    for (const auto& s : satmisc::satSystems()) t.push_back(satDetectTuning(s.stdMode));
    t.push_back(navicTuning());
    for (int k = 0; k < kCisHfKinds; k++) { ModeTuning m = cisHfTuning(k); m.minMhz = 0.001; m.maxMhz = kMaxTuneMhz; t.push_back(m); }   // CIS HF modes (families 620 - 626), any frequency
    for (int k = 0; k < kW1aKinds; k++) { ModeTuning m = w1aTuning(k); m.minMhz = 0.001; m.maxMhz = kMaxTuneMhz; t.push_back(m); }   // W1A voiceband modes (families 800 - 802), any frequency
    t.push_back(hfUtilTuning(kHfuKindHdr));   // STANAG 4539 / 110C high rate (family 603)
    t.push_back(hfUtilTuning(kHfuKindAle3g));   // 3G-ALE (family 604)
    t.push_back(hfUtilTuning(kHfuKindLink11));   // Link 11 CLEW (family 605)
    t.push_back(hfUtilTuning(kHfuKind4481));   // STANAG 4481 FSK (family 606)
    return t;
}
static const std::vector<ModeTuning>& table() {
    static const std::vector<ModeTuning> t = withSatDetect(anyFrequency({dvbsTuning(), dtmbTuning(), atvTuning(), dmrTuning(), drmTuning(), adsbTuning(), gnssTuning(), sondeTuning(), aisTuning(), marineTuning(), acarsTuning(), inmcTuning(), aeroTuning(), iridiumTuning(), meshTuning(),
                                               wxsatTuning(0), wxsatTuning(1), wxsatTuning(2), wxsatTuning(3), wxsatTuning(4), wxsatTuning(5), wxsatTuning(6), wxsatTuning(7), wxsatTuning(8), wxsatTuning(9), wxsatTuning(10), wxsatTuning(11), rcLinkTuning(), hdrTuning(), cdrTuning(), pagerTuning(), packetTuning(), hfdigTuning(), airbandTuning(), vdl2Tuning(), ismTuning(), vorTuning(), m17Tuning(), cellTuning(), cwTuning(), dstarTuning(), hfdlTuning(), nxdnTuning(), dpmrTuning(), gsmrTuning(), pskTuning(), js8Tuning(), specxTuning(), specanTuning(), dvbcTuning(), orbcommTuning(), uplinkTuning(), tetraTuning(), aleTuning(), macTuning(), astroTuning(), p25Tuning(), bleTuning(), ionoTuning(), qo100Tuning(), wifiTuning(), generalTuning(), timeTuning(), ridTuning(), meteorTuning(), nrTuning(), cubesatTuning(), goesTuning(), globalstarTuning(), vdesTuning(), hrptTuning(), zigbeeTuning(), threadTuning(), flarmTuning(), hfbeaconTuning(), djiTuning(), nbiotTuning(), halowTuning(), codarTuning(), wbTuning(), datawellTuning(), multitoneTuning(), sameTuning(), loraRawTuning(), antTuning(), dectTuning(), tetrapolTuning(), elektroTuning(), gk2aLritTuning(), gk2aHritTuning(), ndbTuning(), lbjTuning(), klwTuning(), tdcsTuning(), horusTuning(), dgpsTuning(), vdl4Tuning(), wisunTuning(), uatTuning(), eczasTuning(), wmbusTuning(), tonesTuning(), sarsatTuning(), mt63Tuning(), fsqTuning(), ifkpTuning(), kaalTuning(), kavbTuning(), kappTuning(), kaftTuning(), zwaveTuning(), mscatterTuning(), fpvTuning(), cquamTuning(), dmeTuning(), sitorTuning(), elLoranTuning(), mpt1327Tuning(), fusionTuning(), ardopTuning(), goesdcsTuning(), hrdcpTuning(), modeacTuning(), bletrkTuning(), kaitTuning(), satbeaconTuning(), kawsTuning(), milhfTuning(), sbasTuning(), asterixTuning(), ltrTuning(), edacsTuning(), lorawanTuning(), loraSatTuning(), pactorTuning(), esubghzTuning(), roshfTuning(), ssdvTuning(), ebicabTuning(), inmsatTuning(), iptvTuning(), nrf24Tuning(), atisTuning(), fy3AhrptTuning(), fy3MptTuning(), pactor2Tuning(), pactor3Tuning(), pactor4Tuning(), pskdTuning(), p25p2Tuning(), isdbsTuning(), marineVhfTuning(), selcalTuning(), argosTuning(), sigfoxTuning(), nfcTuning(), shortwaveTuning(), fy4LritTuning(), fy4aHritTuning(), fy2SvissrTuning(), sensorsTuning(), bganTuning(), sfericsTuning(), ltposTuning(), adslTuning(), smartnetTuning(), tetraDmoTuning(), openSkyTuning(), btcTuning(), chanVoiceTuning(chv::Service::Cb), chanVoiceTuning(chv::Service::Frs), chanVoiceTuning(chv::Service::Pmr), chanVoiceTuning(chv::Service::Ham), chanVoiceTuning(chv::Service::HfVoice), chanVoiceTuning(chv::Service::Mdc), chanVoiceTuning(chv::Service::Mic), avRoadTuning(0), avRoadTuning(1), avRoadTuning(2), avRoadTuning(3), avRoadTuning(4), avRoadTuning(5), avRoadTuning(6), avRoadTuning(7), avRoadTuning(8), avRoadTuning(9), avRoadTuning(10), avRoadTuning(11), x40Tuning(0), x40Tuning(1), x40Tuning(2), x40Tuning(3), x40Tuning(4), x40Tuning(5), x40Tuning(6), x40Tuning(7), x40Tuning(8), x40Tuning(9), x40Tuning(10), x40Tuning(11), x40Tuning(12), x40Tuning(13), x40Tuning(14), x40Tuning(15), x40Tuning(16), x40Tuning(17), x40Tuning(18), x40Tuning(19), x40Tuning(20), x40Tuning(21), x40Tuning(22), x40Tuning(23), x40Tuning(24), x40Tuning(25), x40Tuning(26), x40Tuning(27), x40Tuning(28), x40Tuning(29), x40Tuning(30), x40Tuning(31), x40Tuning(32), x40Tuning(33), x40Tuning(34), x40Tuning(35), x40Tuning(36), x40Tuning(37), x40Tuning(38), x40Tuning(39), x40Tuning(40), x40Tuning(41), x40Tuning(42), x40Tuning(43), x40Tuning(44), x40Tuning(45), sd3Tuning(0), sd3Tuning(1), sd3Tuning(2), sd3Tuning(3), sd3Tuning(4), sd3Tuning(5), sd3Tuning(6), sd3Tuning(7), sd3Tuning(8), sd3Tuning(9), sd3Tuning(10), hfUtilTuning(0), hfUtilTuning(1), hfUtilTuning(2), hfUtilTuning(3), hfUtilTuning(4), hfUtilTuning(5), hfUtilTuning(6), hfUtilTuning(7), hfUtilTuning(8), hfUtilTuning(9), hamsciTuning(0), hamsciTuning(1), hamsciTuning(2), hamsciTuning(3), hamsciTuning(4), hamsciTuning(5), hamsciTuning(6), hamsciTuning(7), hamsciTuning(8), hamsciTuning(9), hamsciTuning(10), hamsciTuning(11), hamsciTuning(12), hamsciTuning(13), hamsciTuning(14), hamsciTuning(15), xbandTuning(0), xbandTuning(1), xbandTuning(2), xbandTuning(3), xbandTuning(4), xbandTuning(5), xbandTuning(6), xbandTuning(7), xbandTuning(8), xbandTuning(9), xbandTuning(10), xbandTuning(11), xbandTuning(12), xbandTuning(13), xbandTuning(14), xbandTuning(15), xbandTuning(16), xbandTuning(17), xbandTuning(18), xbandTuning(19), xlinkTuning(), xlinkNarrowTuning(), umtsTuning(), isdbtsbTuning(), cdmaTuning(), tdscdmaTuning(), vdl3Tuning(), cp16Tuning(), dvbshTuning(), dvbc2Tuning(), cubesatViewTuning(0), cubesatViewTuning(1), cubesatViewTuning(2), cubesatViewTuning(3), cubesatViewTuning(4), cubesatViewTuning(5), cubesatViewTuning(6), cubesatViewTuning(7), cubesatViewTuning(8), sensorsViewTuning(0), sensorsViewTuning(1), sensorsViewTuning(2), sensorsViewTuning(3), sensorsViewTuning(4), sensorsViewTuning(5), sensorsViewTuning(6), sensorsViewTuning(7), sensorsViewTuning(8), sensorsViewTuning(9), sensorsViewTuning(10), sensorsViewTuning(11), sensorsViewTuning(12), sensorsViewTuning(13), sensorsViewTuning(14), sensorsViewTuning(15), sensorsViewTuning(16), sensorsViewTuning(17), sensorsViewTuning(18), sensorsViewTuning(19), sensorsViewTuning(20), sensorsViewTuning(21), sondeViewTuning(0), sondeViewTuning(1), sondeViewTuning(2), sondeViewTuning(3), sondeViewTuning(4), sondeViewTuning(5), sondeViewTuning(6), sondeViewTuning(7), sondeViewTuning(8), sondeViewTuning(9), wmbusViewTuning(0), wmbusViewTuning(1), wmbusViewTuning(2), wmbusViewTuning(3), wmbusViewTuning(4), wmbusViewTuning(5), wmbusViewTuning(6), wmbusViewTuning(7), wmbusViewTuning(8), nfcViewTuning(0), nfcViewTuning(1), nfcViewTuning(2), aisViewTuning(0), aisViewTuning(1), aisViewTuning(2), aisViewTuning(3), aisViewTuning(4), aisViewTuning(5), timeViewTuning(0), timeViewTuning(1), timeViewTuning(2), timeViewTuning(3), timeViewTuning(4), timeViewTuning(5), timeViewTuning(6), timeViewTuning(7), timeViewTuning(8), timeViewTuning(9), timeViewTuning(10), gnssViewTuning(0), gnssViewTuning(1), gnssViewTuning(2), gnssViewTuning(3), atvViewTuning(0), atvViewTuning(1), atvViewTuning(2), atvViewTuning(3), atvViewTuning(4), atvViewTuning(5), atvViewTuning(6), atvViewTuning(7), fbrTuning()}));
    return t;
}

namespace {
struct PluginTuningEntry { ModeTuning t; std::string id, name; };
std::mutex gPluginTuningMu;
std::map<int, PluginTuningEntry>& pluginTunings() { static std::map<int, PluginTuningEntry> m; return m; }   // node-based: the pointers stay valid
}

void registerPluginTuning(int stdMode, const std::string& id, const std::string& name, double minMhz, double maxMhz, double defMhz, double sampleRate, double bandwidthMhz) {
    if (!isPluginStd(stdMode)) return;
    std::lock_guard<std::mutex> lk(gPluginTuningMu);
    auto& e = pluginTunings()[stdMode];
    e.id = id; e.name = name;
    e.t = ModeTuning{};
    e.t.stdMode = stdMode; e.t.id = e.id.c_str(); e.t.name = e.name.c_str();
    e.t.minMhz = minMhz; e.t.maxMhz = maxMhz; e.t.defMhz = defMhz; e.t.sampleRate = sampleRate; e.t.bandwidthMhz = bandwidthMhz;
    e.t.minSampleRate = std::min(sampleRate, 250e3);
}

const std::vector<ModeTuning>& modeTunings() { return table(); }

const ModeTuning* modeTuning(int stdMode) {
    if (isPluginStd(stdMode)) {
        std::lock_guard<std::mutex> lk(gPluginTuningMu);
        auto it = pluginTunings().find(stdMode);
        return it == pluginTunings().end() ? nullptr : &it->second.t;
    }
    for (const auto& m : table()) if (m.stdMode == stdMode) return &m;
    return nullptr;
}

// the protocols listed as modes of their own (mode_tuning.h): RTTY, SSTV, FreeDV, the WSJT modes (FT8 .. MSK144) and HamDRM of HF digital, the five LoRa networks
// of mesh, POCSAG and FLEX, the three marine services, and VOR and ILS
static const SubMode kSubModes[] = {
    {"rtty",       "RTTY",       27, 1, -1, 14.080,  {0, 0, 0}},
    {"sstv",       "SSTV",       27, 2, -1, 14.230,  {1, 0, 0}},
    {"freedv",     "FreeDV",     27, 4, -1, 14.236,  {2, 0, 0}},
    {"ft8",        "FT8",        27, 8, 0,  14.074,  {3, 0, 0}},
    {"ft4",        "FT4",        27, 8, 1,  14.080,  {4, 0, 0}},
    {"ft2",        "FT2",        27, 8, 2,  14.084,  {6, 0, 0}},
    {"wspr",       "WSPR",       27, 8, 3,  14.0956, {5, 0, 0}},
    {"hamdrm",     "HamDRM",     27, 16, -1, 14.233, {7, 0, 0}},
    {"meshtastic", "Meshtastic", 22, 1, 0,  869.525, {1, 0, 0}},
    {"meshcore",   "MeshCore",   22, 2, 0,  869.618, {2, 0, 1}},
    {"loraaprs",   "LoRa APRS",  22, 4, 0,  433.775, {4, 2, 1}},
    {"meshcom",    "MeshCom",    22, 8, 0,  433.175, {8, 2, 0}},
    {"lorawan",    "LoRaWAN",    22, 16, 0, 868.1,   {16, 3, 0}},
    {"reticulum",  "Reticulum",  362, 1,  0, 868.0,   {0, 0, 0}},   // RNode LoRa
    {"elrs",       "ExpressLRS", 362, 2,  0, 866.425, {0, 0, 0}},   // centre of the EU868 hop plan
    {"mavlink",    "MAVLink (SiK)", 362, 4, 0, 433.92, {0, 0, 0}},
    {"frsky",      "FrSky",      362, 8,  0, 2440.0,  {0, 0, 0}},
    {"flysky",     "FlySky",     362, 16, 0, 2440.0,  {0, 0, 0}},
    {"sfhss",      "Futaba S-FHSS", 362, 32, 0, 2440.0, {0, 0, 0}},
    {"spektrum",   "Spektrum DSM", 362, 64, 0, 2440.0, {0, 0, 0}},
    {"wfbng",      "wfb-ng / OpenHD", 602, 1, 0, 5805.0, {0, 0, 0}},
    {"npr",        "New Packet Radio", 603, 4, 0, 437.0, {0, 0, 0}},
    {"winmor",     "WINMOR",     603, 8, 0, 7.1,    {0, 0, 0}},   // dial frequency, upper sideband
    {"mioty",      "mioty",      603, 16, 0, 868.18, {0, 0, 0}},   // EU0 / EU1 channel A
    {"dectnrp",    "DECT NR+",   603, 32, 0, 1881.792, {0, 0, 0}},   // channel 1657 (450.144 + n x 0.864 MHz)
    {"pocsag",     "POCSAG",     25, 1, 0,  466.075, {1, 0, 0}},
    {"flex",       "FLEX",       25, 2, 0,  929.6625, {2, 0, 0}},
    {"navtex",     "NAVTEX",     17, 0, 1,  0.518,   {1, 0, 0}},
    {"dsc",        "DSC",        17, 0, 2,  2.1875,  {2, 0, 0}},
    {"wefax",      "Weather fax", 17, 0, 3, 7.8781,  {3, 0, 0}},   // DWD Pinneberg 7880 kHz: the USB dial 1.9 kHz below the carrier
    {"vor",        "VOR",        32, 1, 1, 113.000, {0, 0, 0}},   // sel: VorReceiver::setFunction
    {"ils",        "ILS",        32, 2, 2, 110.300, {2, 0, 0}},
    // more of the WSJT-X family (HF digital, the FT8 / WSPR decoders): JT65, JT9, Q65, FST4 (also FST4W) and MSK144
    {"jt65",       "JT65",       27, 8, 4,  14.076,  {21, 0, 0}},
    {"jt9",        "JT9",        27, 8, 5,  14.078,  {22, 0, 0}},
    {"q65",        "Q65",        27, 8, 6,  14.1,    {23, 0, 0}},
    {"fst4",       "FST4",       27, 8, 7,  1.8366,  {24, 0, 0}},
    {"msk144",     "MSK144",     27, 8, 8,  28.145,  {25, 0, 0}},
    {"jt4",        "JT4",        27, 8, 10, 144.120, {32, 0, 0}},   // EME / weak signal, sub-mode A .. G
    // the keyboard modes of HF digital (hfdig_kbd.h): decoder bit 32, sel = the family (KbdFamily)
    {"olivia",     "Olivia",     27, 32, 0, 14.1065, {26, 0, 0}},
    {"contestia",  "Contestia",  27, 32, 1, 14.1095, {27, 0, 0}},
    {"mfsk",       "MFSK",       27, 32, 2, 14.0785, {28, 0, 0}},
    {"thor",       "THOR",       27, 32, 3, 14.0770, {29, 0, 0}},
    {"dominoex",   "DominoEX",   27, 32, 4, 14.0715, {30, 0, 0}},
    {"hell",       "Hellschreiber", 27, 32, 5, 14.0630, {31, 0, 0}},
    // AM broadcast (MW / SW): the general receiver (57) opened in AM with the MW and SW presets; the plain General receiver stays its own entry
    {"am",         "AM broadcast", 57, 0, 0, 0.999, {0, 0, 0}},
};
const SubMode* subModes(int& count) { count = (int)(sizeof kSubModes / sizeof *kSubModes); return kSubModes; }
const SubMode* subModeById(const std::string& id) {
    for (const auto& m : kSubModes) if (id == m.id) return &m;
    return nullptr;
}
void applySubModeSynth(SynthConfig& sc, const SubMode& s) {
    if (!(s.stdMode == 17 && s.sel == 2 && sc.modeOpt[0] == 4)) sc.modeOpt[0] = s.synth[0];   // DSC keeps VHF channel 70 when chosen
    if (s.stdMode == 22) { sc.modeOpt[1] = s.synth[1]; sc.modeOpt[4] = s.synth[2]; }   // mesh: the band's scene, placed for this network's frequency
    if (s.stdMode == 17) {   // marine: the slots the marine panel does not show are the defaults, not what another mode left there
        // (ADS-B "emergency" in modeOpt[3] made the fax 1 lpm, its reference latitude in modeVal[2] the phasing length, GNSS's tune in
        // modeOpt[5] the picture height, the spectrum analyser's edges in modeVal[0] the mistuning)
        for (int i = 2; i < 8; i++) sc.modeOpt[i] = 0;
        if (sc.modeOpt[1] != 1) sc.modeOpt[1] = 0;
        if (std::fabs(sc.modeVal[0]) > 100) sc.modeVal[0] = 0;
        for (int i = 1; i < 4; i++) sc.modeVal[i] = 0;
    }
}

const ModeTuning* modeTuningById(const std::string& id) {
    for (const auto& m : table()) if (id == m.id) return &m;
    return nullptr;
}

double minSampleRateFor(int stdMode, double bandwidthMhz) {
    if (const ModeTuning* m = modeTuning(stdMode)) return m->minSampleRate;
    switch (stdMode) {
    case 0: case 1: case 2: return 7.9e6 * bandwidthMhz / 8.0;   // T2Receiver / DvbtReceiver::configure(): rateOk
    case 3: return 6.5e6;    // ATSC 8-VSB: AtscReceiver (ok = fin >= 6.5 MHz)
    case 4: return 1.6e6;    // DAB: the ensemble is 1.536 MHz wide, the receiver resamples to 2.048 Msps
    case 5: return 6.5e6;    // ATSC 3.0: about 6.5 Msps for the 6 MHz channel
    case 6: return 6.0e6 * std::max(6.0, std::min(8.0, bandwidthMhz)) / 6.0;   // ISDB-T: IsdbtReceiver (rateOk: 6 Msps per 6 MHz of channel)
    case 7: return 500e3;    // FM: FmReceiver (ready from 500 ksps)
    default: return 0;
    }
}

std::unique_ptr<ModeSynth> makeModeSynth(int stdMode, const SynthConfig& cfg, double sampleRate) {
    switch (stdMode) {
    case 4: return makeDabSynth(cfg, sampleRate);
    case 3: if (sampleRate < 6.5e6 || sampleRate > 2 * atsc::kSymbolRate) return nullptr; return std::make_unique<AtscSynth>(cfg, sampleRate);
    case 5: return makeAtsc3Synth(cfg, sampleRate);
    case 6: return std::make_unique<IsdbtSynth>(cfg, sampleRate);
    case 732: return std::make_unique<IsdbtSynth>(cfg, sampleRate, true);   // ISDB-TSB
    case 743: return makeDvbc2Synth(cfg, sampleRate);   // DVB-C2
    case 7: return std::make_unique<FmSynth>(cfg, sampleRate);
    case 8: return makeDvbsSynth(cfg, sampleRate);
    case 9: return makeDtmbSynth(cfg, sampleRate);
    case 10: return makeAtvSynth(cfg, sampleRate);
    case 426: case 429: case 443: case 446: case 449: case 452: case 455: case 458: {   // the analog TV standard entries: that standard's test signal
        SynthConfig c1 = cfg;
        if (const AtvView* tv = atvViewForStd(stdMode)) { c1.modeOpt[0] = tv->sys == kAtvG ? 0 : tv->sys; c1.modeOpt[1] = tv->colour == kAtvMono ? 4 : tv->colour; }   // the generator's system numbers: 0 B/G (8 MHz), 1 B, then the AtvSys values; colour 4 is monochrome
        return makeAtvSynth(c1, sampleRate);
    }
    case 11: return makeDmrSynth(cfg, sampleRate);
    case 12: return makeDrmSynth(cfg, sampleRate);
    case 13: return makeAdsbSynth(cfg, sampleRate);
    case 14: return makeGnssSynth(cfg, sampleRate);
    case 862: case 868: case 874: case 877: {   // the satellite system entries: that system's satellites in its band
        SynthConfig c1 = cfg;
        if (const GnssView* gv = gnssViewForStd(stdMode)) { c1.modeOpt[0] = (int)gnssSystemBit(gv->system); c1.modeOpt[5] = (int)std::lround(gv->defMhz * 1000.0); }
        return makeGnssSynth(c1, sampleRate);
    }
    case 15: return makeSondeSynth(cfg, sampleRate);
    case 686: case 689: case 692: case 695: case 704: case 707: case 710: case 713: case 716: case 719: {   // the radiosonde type entries: one sonde of that type
        static const int kTypeBit[11] = {0, 1, 16, 2, 4, 8, 32, 64, 128, 256, 512};   // by SondeKind: the sonde_gen type mask (bit 0 RS41, 1 DFM, 2 M10, 3 M20, 4 RS92, then LMS6, iMet, iMet-54, MRZ, iMS-100)
        SynthConfig c1 = cfg;
        if (const SondeView* sv = sondeViewForStd(stdMode)) { c1.modeOpt[0] = kTypeBit[sv->kind]; c1.modeOpt[1] = 1; }
        return makeSondeSynth(c1, sampleRate);
    }
    case 16: return makeAisSynth(cfg, sampleRate);
    case 740: case 809: case 812: case 815: case 818: case 859: return makeAisSynth(cfg, sampleRate);
    case 17: return makeMarineSynth(cfg, sampleRate);
    case 121: return makeMarineSynthNavtex(cfg, sampleRate, "");   // SITOR-B: the NAVTEX demo messages are plain FEC text
    case 18: return makeAcarsSynth(cfg, sampleRate);
    case 19: return makeInmcSynth(cfg, sampleRate);
    case 20: return makeAeroSynth(cfg, sampleRate);
    case 21: return makeIridiumSynth(cfg, sampleRate);
    case 22: return makeMeshSynth(cfg, sampleRate);
    case 23: return makeHdrSynth(cfg, sampleRate);
    case 24: return makeCdrSynth(cfg, sampleRate);
    case 25: return makePagerSynth(cfg, sampleRate);
    case 26: return makePacketSynth(cfg, sampleRate);
    case 27: return makeHfdigSynth(cfg, sampleRate);
    case 28: return makeAirbandSynth(cfg, sampleRate);
    case 29: return makeVdl2Synth(cfg, sampleRate);
    case 84: return makeVdl4Synth(cfg, sampleRate);
    case 30: return makeIsmSynth(cfg, sampleRate);
    case 31: return makeNdbSynth(cfg, sampleRate);
    case 32: return makeVorSynth(cfg, sampleRate);
    case 33: return makeM17Synth(cfg, sampleRate);
    case 37: return makeHfdlSynth(cfg, sampleRate);
    case 34: return makeCellSynth(cfg, sampleRate);
    case 36: return makeCwSynth(cfg, sampleRate);
    case 35: return makeDstarSynth(cfg, sampleRate);
    case 40: return makeNxdnSynth(cfg, sampleRate);
    case 41: return makeGsmrSynth(cfg, sampleRate);
    case 42: return makeDvbcSynth(cfg, sampleRate);
    case 47: return makeDpmrSynth(cfg, sampleRate);
    case 49: return makeMacSynth(cfg, sampleRate);
    case 46: return makeOrbcommSynth(cfg, sampleRate);
    case 48: return makeTetraSynth(cfg, sampleRate);
    case 45: return makeUplinkSynth(cfg, sampleRate);
    case 51: return makeAleSynth(cfg, sampleRate);
    case 52: return makeP25Synth(cfg, sampleRate);
    case 53: return makeBleSynth(cfg, sampleRate);
    case 55: return makeQo100Synth(cfg, sampleRate);
    case 56: return makeWifiSynth(cfg, sampleRate);
    case 57: return makeGeneralSynth(cfg, sampleRate);
    case 60: return makeTimeSynth(cfg, sampleRate);
    case 609: case 612: case 615: case 618: case 630: case 633: case 636: case 639: case 644: case 647: case 650: {   // the time station entries: that station's test signal
        SynthConfig c1 = cfg;
        if (const TimeView* tv = timeViewForStd(stdMode)) c1.modeOpt[0] = tv->station + 1;
        return makeTimeSynth(c1, sampleRate);
    }
    case 63: return makeCubesatSynth(cfg, sampleRate);
    case 842: case 845: case 848: case 851: case 854: case 857: case 794: case 797: case 806: {   // the framing entries: the test signal of that framing only
        const CubesatView* cv = cubesatViewForStd(stdMode);
        SynthConfig c1 = cfg;
        c1.modeOpt[0] = cv ? 1 + cv->framing : 0;
        return makeCubesatSynth(c1, sampleRate);
    }
    case 64: return makeGlobalstarSynth(cfg, sampleRate);
    case 65: return makeVdesSynth(cfg, sampleRate);
    case 67: return makeZigbeeSynth(cfg, sampleRate);
    case 246: return makeZigbeeSynth(cfg, sampleRate);   // Thread (family 244): the same 802.15.4 test signal
    case 68: return makeFlarmSynth(cfg, sampleRate);
    case 302: return makeAdslSynth(cfg, sampleRate);   // ADS-L (family 300)
    case 69: return makeHfbeaconSynth(cfg, sampleRate);
    case 70: return makeDjiSynth(cfg, sampleRate);
    case 71: return makeNbiotSynth(cfg, sampleRate);
    case 72: return makeHalowSynth(cfg, sampleRate);
    case 74: return makeCodarSynth(cfg, sampleRate);
    case 76: return makeDatawellSynth(cfg, sampleRate);
    case 78: return makeSameSynth(cfg, sampleRate);
    case 79: return makeAntSynth(cfg, sampleRate);
    case 80: return makeDectSynth(cfg, sampleRate);
    case 81: return makeTetrapolSynth(cfg, sampleRate);
    case 82: return makeElektroSynth(cfg, sampleRate);
    case 83: return makeLbjSynth(cfg, sampleRate);
    case railway::kKlwStandard: return makeRailwaySynth(cfg, sampleRate, railway::Protocol::Klw);
    case railway::kTdcsStandard: return makeRailwaySynth(cfg, sampleRate, railway::Protocol::Tdcs);
    case horus::kStandard: return makeHorusSynth(cfg, sampleRate);
    case 85: return makeUatSynth(cfg, sampleRate);
    case 77: return makeMultitoneSynth(cfg, sampleRate);
    case 73: return makeLoraRawSynth(cfg, sampleRate);
    case 202: return makeLorawanSynth(cfg, sampleRate);
    case 203: return makeLoraSatSynth(cfg, sampleRate);
    case 205: return makePactorSynth(cfg, sampleRate);
    case 432: case 433: case 434: case 435: case 436: case 437: case 438: case 439: case 440: case 441: return makeHfUtilSynth(stdMode - 432, cfg, sampleRate);
    case kHfuHdrFamily + 2: return makeHfUtilSynth(kHfuKindHdr, cfg, sampleRate);
    case kHfuAle3gFamily + 2: return makeHfUtilSynth(kHfuKindAle3g, cfg, sampleRate);
    case kHfuLink11Family + 2: return makeHfUtilSynth(kHfuKindLink11, cfg, sampleRate);
    case kHfu4481Family + 2: return makeHfUtilSynth(kHfuKind4481, cfg, sampleRate);
    case 239: return makePactorHfSynth(2, cfg, sampleRate);
    case 240: return makePactorHfSynth(3, cfg, sampleRate);
    case 241: return makePactorHfSynth(4, cfg, sampleRate);
    case 206: return makeEsubghzSynth(cfg, sampleRate);
    case 207: return makeRoshfSynth(cfg, sampleRate);
    case 208: return makeSsdvSynth(cfg, sampleRate);
    case 209: return makeEbicabSynth(cfg, sampleRate);
    case 210: return makeInmsatSynth(cfg, sampleRate);
    case 211: return makeIptvSynth(cfg, sampleRate);   // Internet TV (family 209): no samples
    case 212: return makeNrf24Synth(cfg, sampleRate);
    case 213: return makeAtisSynth(cfg, sampleRate);
    case 242: return makeSelcalSynth(cfg, sampleRate);
    case 482: return makeUmtsSynth(cfg, sampleRate);
    case 662: return makeCdmaSynth(cfg, sampleRate);
    case 663: return makeTdscdmaSynth(cfg, sampleRate);
    case 642: return makeVdl3Synth(cfg, sampleRate);
    case 882: return makeCp16Synth(cfg, sampleRate);
    case 243: return makeSensorsSynth(cfg, sampleRate);   // Sub-GHz sensors (family 241)
    case 566:     case 569:     case 572:     case 575:     case 578:     case 581:     case 584:     case 587:     case 590:     case 593:     case 596:     case 665:     case 668:     case 671:     case 674:     case 677:     case 680:     case 683: case 414: case 417: case 420: case 423: return makeSensorsSynth(cfg, sampleRate);   // the sensor-family entries
    case 247: return makeBganSynth(cfg, sampleRate);   // Inmarsat BGAN (family 245)
    case 245: return makeSfericsSynth(cfg, sampleRate);   // Sferics (family 243)
    case 792: return ltpos::makeLtposSynth(cfg, sampleRate);   // Lightning position (family 790)
    case 872: return makeFbrSynth(cfg, sampleRate);   // Fleet Broadcast (family 870)
    case 292: return makeSd3Synth(cfg, sampleRate, 0);
    case 293: return makeSd3Synth(cfg, sampleRate, 1);
    case 294: return makeSd3Synth(cfg, sampleRate, 2);
    case 295: return makeSd3Synth(cfg, sampleRate, 3);
    case 296: return makeSd3Synth(cfg, sampleRate, 4);
    case 297: return makeSd3Synth(cfg, sampleRate, 5);
    case 298: return makeSd3Synth(cfg, sampleRate, 6);
    case 299: return makeSd3Synth(cfg, sampleRate, 7);
    case 300: return makeSd3Synth(cfg, sampleRate, 8);
    case 301: return makeSd3Synth(cfg, sampleRate, 9);
    case 512: return makeSd3Synth(cfg, sampleRate, 10);
    case 462: return makeHamsciSynth(cfg, sampleRate, 0);
    case 463: return makeHamsciSynth(cfg, sampleRate, 1);
    case 464: return makeHamsciSynth(cfg, sampleRate, 2);
    case 465: return makeHamsciSynth(cfg, sampleRate, 3);
    case 466: return makeHamsciSynth(cfg, sampleRate, 4);
    case 467: return makeHamsciSynth(cfg, sampleRate, 5);
    case 502: return makeHamsciSynth(cfg, sampleRate, 6);
    case 503: return makeHamsciSynth(cfg, sampleRate, 7);
    case 504: return makeHamsciSynth(cfg, sampleRate, 8);
    case 752: return makeHamsciSynth(cfg, sampleRate, 9);
    case 822: return makeHamsciSynth(cfg, sampleRate, 10);
    case 825: return makeHamsciSynth(cfg, sampleRate, 11);
    case 828: return makeHamsciSynth(cfg, sampleRate, 12);
    case 831: return makeHamsciSynth(cfg, sampleRate, 13);
    case 834: return makeHamsciSynth(cfg, sampleRate, 14);
    case 837: return makeHamsciSynth(cfg, sampleRate, 15);
    case 532: return makeXbandSynth(cfg, sampleRate, 0);
    case 533: return makeXbandSynth(cfg, sampleRate, 1);
    case 534: return makeXbandSynth(cfg, sampleRate, 2);
    case 535: return makeXbandSynth(cfg, sampleRate, 3);
    case 536: return makeXbandSynth(cfg, sampleRate, 4);
    case 537: return makeXbandSynth(cfg, sampleRate, 5);
    case 538: return makeXbandSynth(cfg, sampleRate, 6);
    case 539: return makeXbandSynth(cfg, sampleRate, 7);
    case 540: return makeXbandSynth(cfg, sampleRate, 8);
    case 541: return makeXbandSynth(cfg, sampleRate, 9);
    case 542: return makeXbandSynth(cfg, sampleRate, 10);
    case 543: return makeXbandSynth(cfg, sampleRate, 11);
    case 544: return makeXbandSynth(cfg, sampleRate, 12);
    case 545: return makeXbandSynth(cfg, sampleRate, 13);
    case 546: return makeXbandSynth(cfg, sampleRate, 14);
    case 547: return makeXbandSynth(cfg, sampleRate, 15);
    case 548: return makeXbandSynth(cfg, sampleRate, 16);
    case 549: return makeXbandSynth(cfg, sampleRate, 17);
    case 550: return makeXbandSynth(cfg, sampleRate, 18);
    case 551: return makeXbandSynth(cfg, sampleRate, 19);
    case 262: return makeWxsatSynth(cfg, sampleRate, 0);
    case 263: return makeWxsatSynth(cfg, sampleRate, 1);
    case 264: return makeWxsatSynth(cfg, sampleRate, 2);
    case 265: return makeWxsatSynth(cfg, sampleRate, 3);
    case 266: return makeWxsatSynth(cfg, sampleRate, 4);
    case 267: return makeWxsatSynth(cfg, sampleRate, 5);
    case 268: return makeWxsatSynth(cfg, sampleRate, 6);
    case 269: return makeWxsatSynth(cfg, sampleRate, 7);
    case 270: return makeWxsatSynth(cfg, sampleRate, 8);
    case 271: return makeWxsatSynth(cfg, sampleRate, 9);
    case 272: return makeWxsatSynth(cfg, sampleRate, 10);
    case 273: return makeWxsatSynth(cfg, sampleRate, 11);
    case 237: return makeFy3Synth(cfg, sampleRate, 0);   // FY-3 AHRPT
    case 238: return makeFy3Synth(cfg, sampleRate, 1);   // FY-3 MPT
    case 214: return makeShortwaveSynth(cfg, sampleRate);
    case 232: return makeGk2aLritSynth(cfg, sampleRate);
    case 233: return makeGk2aHritSynth(cfg, sampleRate);
    case 234: return makeFy4LritSynth(cfg, sampleRate);   // modeOpt[0]: 0 FY-4A, 1 FY-4B
    case 235: return makeFy4aHritSynth(cfg, sampleRate);
    case 236: return makeFy2SvissrSynth(cfg, sampleRate);
    case 204: return makePskdSynth(cfg, sampleRate);
    case 86: return makeWisunSynth(cfg, sampleRate);
    case 100: return makeZwaveSynth(cfg, sampleRate);
    case 382: return makeBtcSynth(cfg, sampleRate);
    case 87: return makeEczasSynth(cfg, sampleRate);
    case 103: return makeFpvSynth(cfg, sampleRate);
    case 88: return makeWmbusSynth(cfg, sampleRate);
    case 722: case 725: case 728: case 734: case 737: case 745: case 799: case 469: case 472: {   // the wireless M-Bus mode entries: the 169 MHz plan for mode N, else the 868 MHz plan
        SynthConfig c1 = cfg;
        if (const WmbusView* wv = wmbusViewForStd(stdMode)) c1.modeOpt[0] = wv->plan169 ? 1 : 0;
        return makeWmbusSynth(c1, sampleRate);
    }
    case 91: return makeTonesSynth(cfg, sampleRate);
    case 94: return makeSarsatSynth(cfg, sampleRate);
    case 154: return makeArgosSynth(cfg, sampleRate);
    case 155: return makeSigfoxSynth(cfg, sampleRate);
    case 97: return makeMt63Synth(cfg, sampleRate);
    case 142: return makeArdopSynth(cfg, sampleRate);
    case 115: return makeFsqSynth(cfg, sampleRate);
    case 118: return makeIfkpSynth(cfg, sampleRate);
    case 124: return makeKaalSynth(cfg, sampleRate);
    case 125: return makeKavbSynth(cfg, sampleRate);
    case 126: return makeKappSynth(cfg, sampleRate);
    case 127: return makeKaftSynth(cfg, sampleRate);
    case 133: return makeKaitSynth(cfg, sampleRate);
    case 135: return makeKawsSynth(cfg, sampleRate);
    case 109: return makeCquamSynth(cfg, sampleRate);
    case 106: return makeMscatterSynth(cfg, sampleRate);
    case 143: return makeGoesDcsSynth(cfg, sampleRate);
    case 702: return makeHrdcpSynth(cfg, sampleRate);
    case 144: return makeModeAcSynth(cfg, sampleRate);
    case 145: return makeBletrkSynth(cfg, sampleRate);
    case 146: return makeSatbeaconSynth(cfg, sampleRate);
    case 563: return makeNavicSynth(cfg, sampleRate);
    case 622: case 623: case 624: case 625: case 626: case 627: case 628: return makeCisHfSynth(stdMode - 622, cfg, sampleRate);   // CIS HF modes
    case 802: case 803: case 804: return makeW1aSynth(stdMode - 802, cfg, sampleRate);   // W1A voiceband modes (families 800 - 802)
    case 552: case 553: case 554: case 555: case 556: case 557: case 558: case 559: case 560: case 561: case 562: case 564: return makeSatDetectSynth(stdMode, cfg, sampleRate);
    case 150: return nfc::makeNfcSynth(cfg, sampleRate);   // NFC / RFID (engine standard 150)
    case 475: case 478: case 484: {   // the NFC protocol entries: that protocol's test session
        SynthConfig c1 = cfg;
        if (const NfcView* nv = nfcViewForStd(stdMode)) c1.modeOpt[0] = nv->proto == 0 ? 0 : nv->proto == 1 ? 2 : 1;   // the generator's sessions: 0 Type A, 1 ISO 15693, 2 Type B
        return nfc::makeNfcSynth(c1, sampleRate);
    }
    case 152: return makeSbasSynth(cfg, sampleRate);
    case 153: return makeAsterixSynth(cfg, sampleRate);
    case 156: return makeIsdbsSynth(cfg, sampleRate);
    case 748: return makeDvbshSynth(cfg, sampleRate);   // DVB-SH
    case 342: case 343: case 344: case 345: case 346: case 347: case 348: return makeChanVoiceSynth(cfg, sampleRate, (chv::Service)(stdMode - 342));   // channelised voice (families 340 - 346)
    case 58: return makeRidSynth(cfg, sampleRate);
    case 61: return makeMeteorSynth(cfg, sampleRate);
    case 59: return makeNrSynth(cfg, sampleRate);
    case 62: return makeGoesSynth(cfg, sampleRate);
    case 66: return makeHrptSynth(cfg, sampleRate);
    case 38: return makePskSynth(cfg, sampleRate);
    case 39: return makeJs8Synth(cfg, sampleRate);
    case 43: return makeSpecxSynth(cfg, sampleRate);
    case 44: return makeSpecanSynth(cfg, sampleRate);
    case 50: return makeAstroSynth(cfg, sampleRate);
    case 54: return makeIonoSynth(cfg, sampleRate);
    default: return nullptr;
    }
}

std::string modeSummary(const RxTelemetry& t) {
    switch (t.standard) {
    case 7: return dvbsSummary(t.dvbs);
    case 8: return dtmbSummary(t.dtmb);
    case 9: return atvSummary(t.atv);
    case 425: case 428: case 442: case 445: case 448: case 451: case 454: case 457: return atvSummary(t.atv);
    case 10: return dmrSummary(t.dmr);
    case 11: return drmSummary(t.drm);
    case 12: return adsbSummary(t.adsb);
    case 13: return gnssSummary(t.gnss);
    case 861: case 867: case 873: case 876: return gnssSummary(t.gnss);
    case 14: return sondeSummary(t.sonde);
    case 685: case 688: case 691: case 694: case 703: case 706: case 709: case 712: case 715: case 718: return sondeSummary(t.sonde);
    case 15: return aisSummary(t.ais);
    case 739: case 808: case 811: case 814: case 817: case 858: return aisSummary(t.ais);
    case 16: return marineSummary(t.marine);
    case 17: return acarsSummary(t.acars);
    case 18: return inmcSummary(t.inmc);
    case 19: return aeroSummary(t.aero);
    case 20: return iridiumSummary(t.iridium);
    case 21: return meshSummary(t.mesh);
    case 22: return hdrSummary(t.hdr);
    case 23: return cdrSummary(t.cdr);
    case 24: return pagerSummary(t.pager);
    case 25: return packetSummary(t.packet);
    case 26: return hfdigSummary(t.hfdig);
    case 27: return airbandSummary(t.airband);
    case 28: return vdl2Summary(t.vdl2);
    case 83: return vdl4Summary(t.vdl4);   // engine standard 83 (stdMode 84)
    case 29: return ismSummary(t.ism);
    case 30: return ndbSummary(t.ndb);
    case 122: return dmeSummary(t.dme);
    case 120: return sitorSummary(t.sitor);
    case 31: return vorSummary(t.vor);
    case 32: return m17Summary(t.m17);
    case 36: return hfdlSummary(t.hfdl);
    case 33: return cellSummary(t.cell);
    case 35: return cwSummary(t.cw);
    case 34: return dstarSummary(t.dstar);
    case 39: return nxdnSummary(t.nxdn);
    case 46: return dpmrSummary(t.dpmr);
    case 48: return macSummary(t.mac);
    case 40: return gsmrSummary(t.gsmr);
    case 41: return dvbcSummary(t.dvbc);
    case 742: return dvbc2Summary(t.dvbc2);
    case 45: return orbcommSummary(t.orbcomm);
    case 47: return tetraSummary(t.tetra);
    case 44: return uplinkSummary(t.uplink);
    case 50: return aleSummary(t.ale);
    case 51: return p25Summary(t.p25);
    case 52: return bleSummary(t.ble);
    case 54: return qo100Summary(t.qo100);
    case 55: return wifiSummary(t.wifi);
    case 56: return generalSummary(t.general);
    case 59: return timeSummary(t.timesig);
    case 608: case 611: case 614: case 617: case 629: case 632: case 635: case 638: case 643: case 646: case 649: return timeSummary(t.timesig);
    case 62: case 841: case 844: case 847: case 850: case 853: case 856: return cubesatSummary(t.cube);
    case 793: case 796: case 805: return cubesatSummary(t.cube);
    case 63: return globalstarSummary(t.globalstar);
    case 64: return vdesSummary(t.vdes);
    case 66: return zigbeeSummary(t.zigbee);
    case 245: return zigbeeSummary(t.thread);
    case 67: return flarmSummary(t.flarm);
    case 301: return adslSummary(t.adsl);
    case 401: case 402: case 403: case 404: case 405: case 406: case 407: case 408: case 409: case 410: case 411: case 412: return t.avroad.headline;
    case 68: return hfbeaconSummary(t.hfb);
    case 69: return djiSummary(t.dji);
    case 70: return nbiotSummary(t.nbiot);
    case 71: return halowSummary(t.halow);
    case 73: return codarSummary(t.codar);
    case 74: return wbSummary(t.wb);
    case 75: return datawellSummary(t.datawell);
    case 77: return sameSummary(t.same);
    case 78: return antSummary(t.ant);
    case 79: return dectSummary(t.dect);
    case 80: return tetrapolSummary(t.tetrapol);
    case 81: return elektroSummary(t.elektro);
    case 82: return lbjSummary(t.lbj);
    case railway::kKlwActiveStandard: return railwaySummary(t.klw, "KLW");
    case railway::kTdcsActiveStandard: return railwaySummary(t.tdcs, "TDCS");
    case horus::kActiveStandard: return horusSummary(t.horus);
    case 121: return dgpsSummary(t.dgps);
    case 127: return elLoranSummary(t.eloran);
    case 128: return mpt1327Summary(t.mpt1327);
    case 129: return fusionSummary(t.fusion);
    case 141: return ardopSummary(t.ardop);
    case 84: return uatSummary(t.uat);
    case 76: return multitoneSummary(t.multitone);
    case 72: return loraRawSummary(t.loraraw);
    case 201: return lorawanSummary(t.lorawan);
    case 202: return loraSatSummary(t.lorasat);
    case 204: return pactorSummary(t.pactor);
    case 205: return esubghzSummary(t.esubghz);
    case 206: return roshfSummary(t.roshf);
    case 207: return ssdvSummary(t.ssdv);
    case 208: return ebicabSummary(t.ebicab);
    case 209: return inmsatSummary(t.inmsat);
    case 361: return rcSummary(t.rclink);
    case 601: case 602: return xlinkSummary(t.xlink);
    case 211: return nrf24Summary(t.nrf24);
    case 212: return atisSummary(t.atis);
    case 241: return selcalSummary(t.selcal);
    case 481: return umtsSummary(t.umts);
    case 661: return cdmaSummary(t.cdma);
    case 662: return tdscdmaSummary(t.tdscdma);
    case 641: return vdl3Summary(t.vdl3);
    case 881: return cp16Summary(t.cp16);
    case 242: return sensorsSummary(t.sensors);
    case 565: case 568: case 571: case 574: case 577: case 580: case 583: case 586: case 589: case 592: case 595: case 664: case 667: case 670: case 673: case 676: case 679: case 682: case 413: case 416: case 419: case 422: return sensorsSummary(t.sensors);
    case 246: return bganSummary(t.bgan);
    case 244: return sfericsSummary(t.sferics);
    case 791: return ltposSummary(t.ltpos);
    DECT2_X40_ACTIVE_CASES return x40Summary(t.x40);   // 4.0 signal modes (families 881 - 884)
    case 871: return fbrSummary(t.fbr);   // Fleet Broadcast (family 870)
    case 531: case 532: case 533: case 534: case 535: case 536: case 537: case 538: case 539: case 540:
    case 541: case 542: case 543: case 544: case 545: case 546: case 547: case 548: case 549: case 550:
        return xbandSummary(t.xband);
    case 261:
    case 262:
    case 263:
    case 264:
    case 265:
    case 266:
    case 267:
    case 268:
    case 269:
    case 270:
    case 271:
    case 272:
        return wxsatSummary(t.wxsat);
    case 291: case 292: case 293: case 294: case 295: case 296: case 297: case 298: case 299: case 300: case 511:
        return sd3Summary(t.sd3);
    case 461:
    case 462:
    case 463:
    case 464:
    case 465:
    case 466:
    case 501:
    case 502:
    case 503:
    case 751:
    case 821: case 824: case 827: case 830: case 833: case 836:
        return hamsciSummary(t.hamsci);
    case 236: return fy3Summary(t.fy3a);
    case 237: return fy3Summary(t.fy3m);
    case 431: case 432: case 433: case 434: case 435: case 436: case 437: case 438: case 439: case 440: return hfUtilSummary(t.hfutil);
    case kHfuHdrFamily + 1: case kHfuAle3gFamily + 1: case kHfuLink11Family + 1: case kHfu4481Family + 1: return hfUtilSummary(t.hfutil);
    case 238: case 239: case 240: return pactorHfSummary(t.pactorhf);
    case 213: return shortwaveSummary(t.shortwave);
    case 231: case 232: return gk2aSummary(t.gk2a);
    case 233: case 234: case 235: return fygeoSummary(t.fygeo);
    case 203: return pskdSummary(t.pskd);
    case 85: return wisunSummary(t.wisun);
    case 99: return zwaveSummary(t.zwave);
    case 381: return btcSummary(t.btc);
    case 86: return eczasSummary(t.eczas);
    case 102: return fpvSummary(t.fpv);
    case 87: return wmbusSummary(t.wmbus);
    case 721: case 724: case 727: case 733: case 736: case 744: case 798: case 468: case 471: return wmbusSummary(t.wmbus);
    case 90: return tonesSummary(t.tones);
    case 93: return sarsatSummary(t.sarsat);
    case 96: return mt63Summary(t.mt63);
    case 114: return fsqSummary(t.fsq);
    case 117: return ifkpSummary(t.ifkp);
    case 123: return kaalSummary(t.kaal);
    case 130: return ltrSummary(t.ltr);
    case 131: return edacsSummary(t.edacs);
    case 321: return smartnetSummary(t.smartnet);
    case 322: return tetraDmoSummary(t.tetradmo);
    case 323: return openSkySummary(t.opensky);
    case 156: return marineVhfSummary(t.marinevhf);
    case 341: case 342: case 343: case 344: case 345: case 346: case 347: return chvSummary(t.chanvoice);
    case 181: return p25p2Summary(t.p25p2);
    case 132: return kaitSummary(t.kait);
    case 124: return kavbSummary(t.kavb);
    case 134: return kawsSummary(t.kaws);
    case 125: return kappSummary(t.kapp);
    case 126: return kaftSummary(t.kaft);
    case 108: return cquamSummary(t.cquam);
    case 105: return msSummary(t.mscatter);
    case 142: return dcsSummary(t.goesdcs);
    case 701: return hrdcpSummary(t.hrdcp);
    case 143: return modeAcSummary(t.modeac);
    case 144: return bletrkSummary(t.bletrk);
    case 146: return milhfSummary(t.milhf);
    case 151: return sbasSummary(t.sbas);
    case 153: return argosSummary(t.argos);
    case 154: return sigfoxModeSummary(t.sigfoxm);
    case 145: return sbSummary(t.satbeacon);
    case 562: return navicSummary(t.navic);
    case 621: case 622: case 623: case 624: case 625: case 626: case 627: return cisHfSummary(t.cishf);   // CIS HF modes
    case 801: case 802: case 803: return w1aSummary(t.w1a);   // W1A voiceband modes
    case 551: case 552: case 553: case 554: case 555: case 556: case 557: case 558: case 559: case 560: case 561: case 563: return sdSummary(t.satdetect);
    case 149: return nfcSummary(t.nfc);
    case 474: case 477: case 483: return nfcSummary(t.nfc);   // NFC / RFID 13.56 MHz (active standard 149)
    case 155: return isdbsSummary(t.isdbs);
    case 747: return dvbshSummary(t.dvbsh);
    case 57: return ridSummary(t.rid);
    case 60: return meteorSummary(t.meteor);
    case 58: return nrSummary(t.nr);
    case 61: return goesSummary(t.goes);
    case 65: return hrptSummary(t.hrpt);
    case 37: return pskSummary(t.psk);
    case 38: return js8Summary(t.js8);
    case 42: return specxSummary(t.specx);
    case 43: return specanSummary(t.specan);
    case 49: return astroSummary(t.astro);
    case 53: return ionoSummary(t.iono);
    default: return "";
    }
}

} // namespace dect2
