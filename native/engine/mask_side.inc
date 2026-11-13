static uint32_t screen_mask_side(hfw::masks* drawable_masks, hfw::masks* offscreen_masks, const int32_t* used, const float* mvp, uint32_t width, uint32_t height, uint32_t current, uint32_t& shrink_frames, uint32_t& shrink_side)
{
    float half_width = width * 0.5f;
    float half_height = height * 0.5f;
    float pixels_x = std::hypot(mvp[0] * half_width, mvp[1] * half_height);
    float pixels_y = std::hypot(mvp[4] * half_width, mvp[5] * half_height);
    float needed = 0.0f;
    for (int kind = 0; kind < 2; kind++)
    {
        hfw::masks* masks = kind == 0 ? drawable_masks : offscreen_masks;
        for (int32_t i = 0; masks != NULL && used[kind] > 0 && i < hfw::masks_group_count(masks); i++)
        {
            const hfw::mask_group* group = hfw::masks_group(masks, i);
            if (group->used)
            {
                needed = std::max({needed, pixels_x * 4.0f / group->mask_draw.m[0], pixels_y * 4.0f / group->mask_draw.m[5]});
            }
        }
    }
    uint32_t side = 256;
    while (side < std::ceil(needed) && side < 4096)
    {
        side *= 2;
    }
    if (side >= current)
    {
        shrink_frames = 0;
        return side;
    }
    shrink_side = shrink_frames == 0 ? side : std::max(shrink_side, side);
    shrink_frames++;
    if (shrink_frames < 60)
    {
        return current;
    }
    shrink_frames = 0;
    return shrink_side;
}
