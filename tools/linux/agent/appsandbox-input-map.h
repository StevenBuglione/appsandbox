#ifndef APPSANDBOX_INPUT_MAP_H
#define APPSANDBOX_INPUT_MAP_H

#include <stdint.h>

#define ASB_INPUT_MIN_FRAME_WIDTH   64u
#define ASB_INPUT_MIN_FRAME_HEIGHT  64u
#define ASB_INPUT_MAX_FRAME_WIDTH   7680u
#define ASB_INPUT_MAX_FRAME_HEIGHT  4320u
#define ASB_INPUT_ABS_RANGE         32767u

struct asb_input_frame_size {
    uint32_t width;
    uint32_t height;
};

static inline int asb_input_frame_size_set(struct asb_input_frame_size *frame,
                                           uint32_t width, uint32_t height)
{
    if (!frame ||
        width < ASB_INPUT_MIN_FRAME_WIDTH ||
        height < ASB_INPUT_MIN_FRAME_HEIGHT ||
        width > ASB_INPUT_MAX_FRAME_WIDTH ||
        height > ASB_INPUT_MAX_FRAME_HEIGHT)
        return 0;

    frame->width = width;
    frame->height = height;
    return 1;
}

static inline int32_t asb_input_scale_axis(uint32_t coordinate,
                                           uint32_t extent)
{
    uint32_t maximum_coordinate;

    if (extent <= 1)
        return 0;

    maximum_coordinate = extent - 1;
    if (coordinate > maximum_coordinate)
        coordinate = maximum_coordinate;

    return (int32_t)((uint64_t)coordinate * ASB_INPUT_ABS_RANGE /
                     maximum_coordinate);
}

#endif
