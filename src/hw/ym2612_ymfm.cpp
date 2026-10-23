// YM2612 via ymfm (BSD-3, Aaron Giles). Timers are counted in chip clocks
// per generated sample, so the chip stays deterministic with the rest of the
// hardware model.
#include <cstring>
#include <vector>
#include "ymfm_opn.h"
#include "ym2612.h"

namespace {

class intf : public ymfm::ymfm_interface {
public:
    int32_t timer[2] = { -1, -1 };   // remaining clocks, -1 = stopped
    void ymfm_set_timer(uint32_t tnum, int32_t duration) override { timer[tnum] = duration; }
    void tick(int32_t clocks)
    {
        for (int t = 0; t < 2; t++) {
            if (timer[t] < 0) continue;
            timer[t] -= clocks;
            if (timer[t] <= 0) {
                int32_t over = -timer[t];
                timer[t] = -1;
                m_engine->engine_timer_expired(t);   // re-arms via ymfm_set_timer
                if (timer[t] > 0) timer[t] -= over;
            }
        }
    }
};

intf g_intf;
ymfm::ym2612 *g_chip;

}  // namespace

extern "C" void ym_init(void)
{
    if (!g_chip) g_chip = new ymfm::ym2612(g_intf);
    ym_reset();
}

extern "C" void ym_reset(void)
{
    g_intf.timer[0] = g_intf.timer[1] = -1;
    g_chip->reset();
}

extern "C" void ym_write(int port, uint8_t v) { g_chip->write(port & 3, v); }

extern "C" uint8_t ym_status(void) { return g_chip->read_status(); }

extern "C" void ym_sample(int32_t out[2])
{
    ymfm::ym2612::output_data o;
    g_chip->generate(&o, 1);
    g_intf.tick(144);
    out[0] = o.data[0];
    out[1] = o.data[1];
}

extern "C" size_t ym_state_save(uint8_t *out)
{
    std::vector<uint8_t> buf;
    ymfm::ymfm_saved_state st(buf, true);
    g_chip->save_restore(st);
    size_t n = buf.size() + sizeof g_intf.timer;
    if (n > YM_STATE_MAX) return 0;
    memcpy(out, g_intf.timer, sizeof g_intf.timer);
    memcpy(out + sizeof g_intf.timer, buf.data(), buf.size());
    return n;
}

extern "C" int ym_state_load(const uint8_t *in, size_t n)
{
    if (n < sizeof g_intf.timer) return 0;
    memcpy(g_intf.timer, in, sizeof g_intf.timer);
    std::vector<uint8_t> buf(in + sizeof g_intf.timer, in + n);
    ymfm::ymfm_saved_state st(buf, false);
    g_chip->save_restore(st);
    return 1;
}
