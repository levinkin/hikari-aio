#include <metal_stdlib>
using namespace metal;

float4 hfw_to_straight(float4 premultiplied)
{
    if (abs(premultiplied.a) < 0.00001) {
        return float4(0.0, 0.0, 0.0, premultiplied.a);
    }
    return float4(premultiplied.rgb / premultiplied.a, premultiplied.a);
}

#if HFW_COLOR_BLEND == 5
static float hfw_lane(float source, float destination)
{
    if (abs(destination - 1.0) < 0.000001) {
        return 1.0;
    }
    if (abs(source) < 0.000001) {
        return 0.0;
    }
    return 1.0 - min(1.0, (1.0 - destination) / source);
}
#elif HFW_COLOR_BLEND == 9
static float hfw_lane(float source, float destination)
{
    if (destination <= 0.0) {
        return 0.0;
    }
    if (source == 1.0) {
        return 1.0;
    }
    return min(1.0, destination / (1.0 - source));
}
#elif HFW_COLOR_BLEND == 10
static float hfw_lane(float source, float destination)
{
    if (destination < 0.5) {
        return 2.0 * source * destination;
    }
    return 1.0 - 2.0 * (1.0 - source) * (1.0 - destination);
}
#elif HFW_COLOR_BLEND == 11
static float hfw_lane(float source, float destination)
{
    if (source <= 0.5) {
        return destination - (1.0 - 2.0 * source) * destination * (1.0 - destination);
    }
    if (destination <= 0.25) {
        return destination + (2.0 * source - 1.0) * destination * ((16.0 * destination - 12.0) * destination + 3.0);
    }
    return destination + (2.0 * source - 1.0) * (sqrt(destination) - destination);
}
#elif HFW_COLOR_BLEND == 12
static float hfw_lane(float source, float destination)
{
    if (source < 0.5) {
        return 2.0 * source * destination;
    }
    return 1.0 - 2.0 * (1.0 - source) * (1.0 - destination);
}
#elif HFW_COLOR_BLEND == 13
static float hfw_lane(float source, float destination)
{
    if (source < 0.5) {
        return max(0.0, 2.0 * source + destination - 1.0);
    }
    return min(1.0, 2.0 * (source - 0.5) + destination);
}
#elif HFW_COLOR_BLEND == 14 || HFW_COLOR_BLEND == 15
static float hfw_luminance(float3 color)
{
    return 0.30 * color.r + 0.59 * color.g + 0.11 * color.b;
}

static float3 hfw_set_luminance(float3 color, float luminance)
{
    float3 shifted = color + (luminance - hfw_luminance(color));
    float level = hfw_luminance(shifted);
    float lowest = min(shifted.r, min(shifted.g, shifted.b));
    float highest = max(shifted.r, max(shifted.g, shifted.b));
    if (lowest < 0.0) {
        shifted = level + (shifted - level) * level / (level - lowest);
    }
    if (highest > 1.0) {
        shifted = level + (shifted - level) * (1.0 - level) / (highest - level);
    }
    return shifted;
}
#endif

#if HFW_COLOR_BLEND == 14
static float3 hfw_set_saturation(float3 color, float saturation)
{
    float highest = max(color.r, max(color.g, color.b));
    float lowest = min(color.r, min(color.g, color.b));
    float middle = color.r + color.g + color.b - highest - lowest;
    float top = lowest < highest ? saturation : 0.0;
    float median = lowest < highest ? (middle - lowest) * saturation / (highest - lowest) : 0.0;
    if (color.r == highest) {
        return color.b < color.g ? float3(top, median, 0.0) : float3(top, 0.0, median);
    }
    if (color.g == highest) {
        return color.r < color.b ? float3(0.0, top, median) : float3(median, top, 0.0);
    }
    return color.g < color.r ? float3(median, 0.0, top) : float3(0.0, median, top);
}
#endif

float3 hfw_blend_color(float3 source, float3 destination)
{
#if HFW_COLOR_BLEND == 1
    return min(source + destination, 1.0);
#elif HFW_COLOR_BLEND == 2
    return source + destination;
#elif HFW_COLOR_BLEND == 3
    return min(source, destination);
#elif HFW_COLOR_BLEND == 4
    return source * destination;
#elif HFW_COLOR_BLEND == 6
    return max(0.0, source + destination - 1.0);
#elif HFW_COLOR_BLEND == 7
    return max(source, destination);
#elif HFW_COLOR_BLEND == 8
    return source + destination - source * destination;
#elif HFW_COLOR_BLEND == 14
    float spread = max(destination.r, max(destination.g, destination.b)) - min(destination.r, min(destination.g, destination.b));
    return hfw_set_luminance(hfw_set_saturation(source, spread), hfw_luminance(destination));
#elif HFW_COLOR_BLEND == 15
    return hfw_set_luminance(source, hfw_luminance(destination));
#elif HFW_COLOR_BLEND >= 5
    return float3(hfw_lane(source.r, destination.r), hfw_lane(source.g, destination.g), hfw_lane(source.b, destination.b));
#else
    return source;
#endif
}

float4 hfw_blend_alpha(float3 color, float4 source, float4 destination)
{
    float source_alpha = source.a;
    float destination_alpha = destination.a;
#if HFW_ALPHA_BLEND == 1
    float both = source_alpha * destination_alpha;
    float source_only = 0.0;
    float destination_only = destination_alpha * (1.0 - source_alpha);
#elif HFW_ALPHA_BLEND == 2
    float both = 0.0;
    float source_only = 0.0;
    float destination_only = destination_alpha * (1.0 - source_alpha);
#elif HFW_ALPHA_BLEND == 3
    float both = min(source_alpha, destination_alpha);
    float source_only = max(source_alpha - destination_alpha, 0.0);
    float destination_only = max(destination_alpha - source_alpha, 0.0);
#elif HFW_ALPHA_BLEND == 4
    float both = max(source_alpha + destination_alpha - 1.0, 0.0);
    float source_only = min(source_alpha, 1.0 - destination_alpha);
    float destination_only = min(destination_alpha, 1.0 - source_alpha);
#else
    float both = source_alpha * destination_alpha;
    float source_only = source_alpha * (1.0 - destination_alpha);
    float destination_only = destination_alpha * (1.0 - source_alpha);
#endif
    float3 rgb = color * both + source.rgb * source_only + destination.rgb * destination_only;
    return float4(rgb, both + source_only + destination_only);
}
