#include <fcntl.h>
#include <math.h>
#include "mcap.h"
#include "../Example.h"

// Bar enum values
#define BAR_PROP0 1
#define BAR_PROP1 2
#define BAR_PROP2 4

// True enum values
#define TRUE_FALSE 0
#define TRUE_TRUE  1

int main(void) {
    int fd = open("example.mcap", O_WRONLY|O_CREAT|O_TRUNC, 0644);
    mcap_writer w;
    mcap_open(&w, fd);

    u16 sid_status = mcap_schema(&w, "example_msgs/msg/Status",
                                 Example_Status_IDL, sizeof(Example_Status_IDL)-1);
    u16 sid_telem  = mcap_schema(&w, "example_msgs/msg/Telemetry",
                                 Example_Telemetry_IDL, sizeof(Example_Telemetry_IDL)-1);

    u16 cid_status = mcap_channel(&w, sid_status, "/example/status");
    u16 cid_telem  = mcap_channel(&w, sid_telem,  "/example/telemetry");

    u64 t = 1748000000000000000ULL; // base timestamp (ns)
    u64 dt = 100000000ULL;          // 100 ms steps

    for (int i = 0; i < 20; i++, t += dt) {
        float phase = (float)i * 0.314f;

        // Status: nested Baz array, foo scalar, bar enum, bat[] bool array
        Example_Status st = {0};
        for (int j = 0; j < 8; j++) {
            st.baz[j].bam = sinf(phase + (float)j * 0.5f) * 5.0f;
            st.baz[j].bop = (u8)(j % 3); // 0,1,2 cycling through Prop values
        }
        st.foo = cosf(phase) * 10.0f;
        st.bar = (u8)(i % 2 == 0 ? BAR_PROP0 : BAR_PROP1 | BAR_PROP2);
        for (int j = 0; j < 8; j++)
            st.bat[j] = (u8)((i + j) % 2 ? TRUE_TRUE : TRUE_FALSE);
        mcap_message(&w, cid_status, t, &st, sizeof(st));

        // Telemetry: Vec3 pos, timestamp, modes[] enum array
        Example_Telemetry tl = {0};
        tl.pos.x = cosf(phase) * 2.0f;
        tl.pos.y = sinf(phase) * 2.0f;
        tl.pos.z = (float)i * 0.1f;
        tl.timestamp = (u32)(i * 100);
        tl.modes[0] = (u8)(i % 2 == 0 ? BAR_PROP0 : 0);
        tl.modes[1] = (u8)(i % 3 == 0 ? BAR_PROP1 : 0);
        tl.modes[2] = (u8)(i % 4 == 0 ? BAR_PROP2 : 0);
        tl.modes[3] = (u8)(BAR_PROP0 | BAR_PROP2);
        mcap_message(&w, cid_telem, t, &tl, sizeof(tl));
    }

    mcap_close(&w);
    close(fd);
    return 0;
}
