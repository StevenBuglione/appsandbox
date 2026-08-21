#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "../appsandbox-input-map.h"

int main(void)
{
    struct asb_input_frame_size frame = { 1920, 1080 };

    assert(!asb_input_frame_size_set(&frame, 63, 600));
    assert(!asb_input_frame_size_set(&frame, 800, 63));
    assert(!asb_input_frame_size_set(&frame, 7681, 600));
    assert(!asb_input_frame_size_set(&frame, 800, 4321));
    assert(frame.width == 1920 && frame.height == 1080);

    assert(asb_input_frame_size_set(&frame, 1689, 905));
    assert(frame.width == 1689 && frame.height == 905);
    assert(asb_input_scale_axis(0, frame.width) == 0);
    assert(asb_input_scale_axis(1688, frame.width) == 32767);
    assert(asb_input_scale_axis(904, frame.height) == 32767);
    assert(asb_input_scale_axis(UINT32_MAX, frame.width) == 32767);

    assert(asb_input_frame_size_set(&frame, 1423, 881));
    assert(asb_input_scale_axis(711, frame.width) == 16383);
    assert(asb_input_scale_axis(440, frame.height) == 16383);

    puts("input frame mapping tests passed");
    return 0;
}
