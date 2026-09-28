"""Compile the production ADC mode builder against small deterministic stubs."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class AdcConfiguration(unittest.TestCase):
    def test_modes_build_channels_and_idle_is_rejected(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / 'src/main.cpp').read_text(encoding='utf-8')
        start = source.index('struct AdcConfigPlan {')
        end = source.index('static esp_err_t commandAdcWorkers', start)
        body = source[start:end]
        harness = r'''
using uint8_t=unsigned char;
enum class Mode : uint8_t { Idle=0, All=1, Raw=2, Env=3, Sensor=4 };
namespace ADS1015 {
enum class ConfigPGA : uint8_t { One };
struct ChannelConfig { uint8_t channel, divider; ConfigPGA gain; };
}
constexpr uint8_t kNUM_SENSORS=8, kSLOW_DIV=20;
constexpr uint8_t kRawCh[4][2]={{0,3},{1,3},{0,3},{1,3}};
constexpr uint8_t kEnvCh[4][2]={{1,2},{0,2},{1,2},{0,2}};
constexpr uint8_t sensorAdc(uint8_t sensor) { return sensor / 2; }
constexpr uint8_t sensorChannel(uint8_t sensor) { return sensor % 4; }
'''
        checks = r'''
#define CHECK(c) do { if (!(c)) return __LINE__; } while(0)
constexpr int run() {
    AdcConfigPlan plan{};
    CHECK(!buildAdcConfigPlan(Mode::Idle, 5, plan));
    CHECK(plan.counts[0][0]+plan.counts[0][1]+
          plan.counts[1][0]+plan.counts[1][1]==0);
    CHECK(buildAdcConfigPlan(Mode::All, 5, plan));
    CHECK(plan.counts[0][0]==4 && plan.counts[0][1]==4 &&
          plan.counts[1][0]==4 && plan.counts[1][1]==4);
    for (uint8_t adc=0; adc<4; ++adc) {
        const uint8_t bus=adc/2, index=adc%2;
        CHECK(plan.configs[bus][index][0].channel==kRawCh[adc][0]);
        CHECK(plan.configs[bus][index][1].channel==kRawCh[adc][1]);
        CHECK(plan.configs[bus][index][2].channel==kEnvCh[adc][0]);
        CHECK(plan.configs[bus][index][3].channel==kEnvCh[adc][1]);
        CHECK(plan.configs[bus][index][0].divider==1 &&
              plan.configs[bus][index][1].divider==1);
        CHECK(plan.configs[bus][index][2].divider==kSLOW_DIV &&
              plan.configs[bus][index][3].divider==kSLOW_DIV);
    }
    CHECK(buildAdcConfigPlan(Mode::Raw, 5, plan));
    CHECK(plan.counts[0][0]==2 && plan.counts[0][1]==2 &&
          plan.counts[1][0]==2 && plan.counts[1][1]==2);
    for (uint8_t adc=0; adc<4; ++adc) {
        const uint8_t bus=adc/2, index=adc%2;
        CHECK(plan.configs[bus][index][0].channel==kRawCh[adc][0]);
        CHECK(plan.configs[bus][index][1].channel==kRawCh[adc][1]);
    }
    CHECK(buildAdcConfigPlan(Mode::Env, 5, plan));
    CHECK(plan.counts[0][0]==2 && plan.counts[0][1]==2 &&
          plan.counts[1][0]==2 && plan.counts[1][1]==2);
    for (uint8_t adc=0; adc<4; ++adc) {
        const uint8_t bus=adc/2, index=adc%2;
        CHECK(plan.configs[bus][index][0].channel==kEnvCh[adc][0]);
        CHECK(plan.configs[bus][index][1].channel==kEnvCh[adc][1]);
        CHECK(plan.configs[bus][index][0].divider==1 &&
              plan.configs[bus][index][1].divider==1);
    }
    CHECK(buildAdcConfigPlan(Mode::Sensor, 5, plan));
    CHECK(plan.counts[0][0]==0 && plan.counts[0][1]==0 &&
          plan.counts[1][0]==1 && plan.counts[1][1]==0);
    CHECK(plan.configs[1][0][0].channel==1);
    return 0;
}
constexpr int result=run();
static_assert(result==0,"ADC configuration regression: result identifies CHECK line");
'''
        compiler = shutil.which('clang++')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as folder:
            cpp = Path(folder) / 'adc_config.cpp'
            cpp.write_text(harness + body + checks, encoding='utf-8')
            subprocess.run([compiler, '-std=c++17', '-fsyntax-only', str(cpp)], check=True)


if __name__ == '__main__':
    unittest.main()
