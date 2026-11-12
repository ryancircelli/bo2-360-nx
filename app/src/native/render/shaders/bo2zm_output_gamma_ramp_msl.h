// Generated from bo2zm_output_gamma_ramp.frag (see the commands there): the output pass in MSL, for the Metal
// presenter. Entry point bo2zm_output_ramp; constants at buffer 0 (16 bytes of padding, then the push constants
// from byte 16), the Ramp block at buffer 2, the source at texture 0, the sampler at sampler 1.
#pragma once
static const char kMslBo2zmOutputRamp[] = R"msl(
#pragma clang diagnostic ignored "-Wmissing-prototypes"
#pragma clang diagnostic ignored "-Wmissing-braces"

#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

template<typename T, size_t Num>
struct spvUnsafeArray
{
    T elements[Num ? Num : 1];
    
    thread T& operator [] (size_t pos) thread
    {
        return elements[pos];
    }
    constexpr const thread T& operator [] (size_t pos) const thread
    {
        return elements[pos];
    }
    
    device T& operator [] (size_t pos) device
    {
        return elements[pos];
    }
    constexpr const device T& operator [] (size_t pos) const device
    {
        return elements[pos];
    }
    
    constexpr const constant T& operator [] (size_t pos) const constant
    {
        return elements[pos];
    }
    
    threadgroup T& operator [] (size_t pos) threadgroup
    {
        return elements[pos];
    }
    constexpr const threadgroup T& operator [] (size_t pos) const threadgroup
    {
        return elements[pos];
    }
};

// Implementation of signed integer mod accurate to SPIR-V specification
template<typename Tx, typename Ty>
inline Tx spvSMod(Tx x, Ty y)
{
    Tx remainder = x - y * (x / y);
    return select(Tx(remainder + y), remainder, remainder == 0 || (x >= 0) == (y >= 0));
}

struct Constants
{
    char _m0_pad[16];
    int2 displacement;
    float2 _inverse;
    uint4 counter;
    uint4 note;
    uint4 banner[2];
    uint4 banner_info;
};

struct Ramp
{
    float4 inputs[256];
    float4 blend;
    float4 effects;
    float4 crosshair_rects[4];
    uint4 crosshair_styles[4];
};

constant bool kFxaa_tmp [[function_constant(2)]];
constant bool kFxaa = is_function_constant_defined(kFxaa_tmp) ? kFxaa_tmp : false;
constant bool kExact_tmp [[function_constant(0)]];
constant bool kExact = is_function_constant_defined(kExact_tmp) ? kExact_tmp : false;
constant bool kGrading_tmp [[function_constant(1)]];
constant bool kGrading = is_function_constant_defined(kGrading_tmp) ? kGrading_tmp : false;

constant spvUnsafeArray<uint, 11> _452 = spvUnsafeArray<uint, 11>({ 31599u, 11415u, 29671u, 29647u, 23497u, 31183u, 31215u, 29257u, 31727u, 31695u, 2u });
constant spvUnsafeArray<float3, 7> _583 = spvUnsafeArray<float3, 7>({ float3(1.0), float3(1.0, 0.1500000059604644775390625, 0.1500000059604644775390625), float3(0.20000000298023223876953125, 1.0, 0.20000000298023223876953125), float3(0.20000000298023223876953125, 0.89999997615814208984375, 1.0), float3(1.0, 0.949999988079071044921875, 0.20000000298023223876953125), float3(1.0, 0.300000011920928955078125, 1.0), float3(1.0, 0.60000002384185791015625, 0.100000001490116119384765625) });
constant spvUnsafeArray<uint, 43> _860 = spvUnsafeArray<uint, 43>({ 0u, 0u, 31599u, 11415u, 29671u, 29647u, 23497u, 31183u, 31215u, 29257u, 31727u, 31695u, 11245u, 27566u, 14627u, 27502u, 31143u, 31140u, 14699u, 23533u, 29847u, 4714u, 23469u, 18727u, 24557u, 27501u, 11114u, 27556u, 11123u, 27565u, 14478u, 29842u, 23407u, 23402u, 23549u, 23213u, 23186u, 29351u, 1040u, 2u, 448u, 21157u, 4772u });

struct bo2zm_output_ramp_out
{
    float4 color [[color(0)]];
};

static inline __attribute__((always_inline))
float Luma(thread const float2& uv, texture2d<float> source, sampler tex_sampler)
{
    return dot(source.sample(tex_sampler, uv, level(0.0)).xyz, float3(0.2989999949932098388671875, 0.58700001239776611328125, 0.114000000059604644775390625));
}

static inline __attribute__((always_inline))
float3 Fxaa(thread const float2& uv, texture2d<float> source, sampler tex_sampler, constant Constants& constants)
{
    float2 _step = constants._inverse;
    float3 rgb_center = source.sample(tex_sampler, uv, level(0.0)).xyz;
    float luma_center = dot(rgb_center, float3(0.2989999949932098388671875, 0.58700001239776611328125, 0.114000000059604644775390625));
    float2 param = uv + float2(-_step.x, -_step.y);
    float luma_nw = Luma(param, source, tex_sampler);
    float2 param_1 = uv + float2(_step.x, -_step.y);
    float luma_ne = Luma(param_1, source, tex_sampler);
    float2 param_2 = uv + float2(-_step.x, _step.y);
    float luma_so = Luma(param_2, source, tex_sampler);
    float2 param_3 = uv + float2(_step.x, _step.y);
    float luma_ = Luma(param_3, source, tex_sampler);
    float luma_min = fast::min(luma_center, fast::min(fast::min(luma_nw, luma_ne), fast::min(luma_so, luma_)));
    float luma_max = fast::max(luma_center, fast::max(fast::max(luma_nw, luma_ne), fast::max(luma_so, luma_)));
    if ((luma_max - luma_min) < fast::max(0.083300001919269561767578125, luma_max * 0.16599999368190765380859375))
    {
        return rgb_center;
    }
    float2 address = float2(-((luma_nw + luma_ne) - (luma_so + luma_)), (luma_nw + luma_so) - (luma_ne + luma_));
    float reduction = fast::max((((luma_nw + luma_ne) + luma_so) + luma_) * 0.03125, 0.0078125);
    float inverse_min = 1.0 / (fast::min(abs(address.x), abs(address.y)) + reduction);
    address = fast::clamp(address * inverse_min, float2(-8.0), float2(8.0)) * _step;
    float3 rgb_a = (source.sample(tex_sampler, (uv + (address * (-0.16666667163372039794921875))), level(0.0)).xyz + source.sample(tex_sampler, (uv + (address * 0.16666667163372039794921875)), level(0.0)).xyz) * 0.5;
    float3 rgb_b = (rgb_a * 0.5) + ((source.sample(tex_sampler, (uv + (address * (-0.5))), level(0.0)).xyz + source.sample(tex_sampler, (uv + (address * 0.5)), level(0.0)).xyz) * 0.25);
    float luma_b = dot(rgb_b, float3(0.2989999949932098388671875, 0.58700001239776611328125, 0.114000000059604644775390625));
    return select(rgb_b, rgb_a, bool3((luma_b < luma_min) || (luma_b > luma_max)));
}

static inline __attribute__((always_inline))
float3 Counter(thread const float3& c, constant Constants& constants, thread float4& gl_FragCoord)
{
    int e = int(max(constants.counter.y, 1u));
    int2 q = int2(gl_FragCoord.xy) - int2(8);
    uint v = min(constants.counter.x, 9999u);
    int _314;
    if (v >= 1000u)
    {
        _314 = 5;
    }
    else
    {
        _314 = (v >= 100u) ? 4 : 3;
    }
    int n = _314;
    bool _329 = q.x < 0;
    bool _336;
    if (!_329)
    {
        _336 = q.y < 0;
    }
    else
    {
        _336 = _329;
    }
    bool _348;
    if (!_336)
    {
        _348 = q.x >= (((n * 4) + 1) * e);
    }
    else
    {
        _348 = _336;
    }
    bool _358;
    if (!_348)
    {
        _358 = q.y >= (7 * e);
    }
    else
    {
        _358 = _348;
    }
    if (_358)
    {
        return c;
    }
    int2 point = (q / int2(e)) - int2(1);
    bool _372 = point.x >= 0;
    bool _378;
    if (_372)
    {
        _378 = point.y >= 0;
    }
    else
    {
        _378 = _372;
    }
    bool _384;
    if (_378)
    {
        _384 = point.y < 5;
    }
    else
    {
        _384 = _378;
    }
    bool _391;
    if (_384)
    {
        _391 = spvSMod(point.x, 4) < 3;
    }
    else
    {
        _391 = _384;
    }
    bool _399;
    if (_391)
    {
        _399 = (point.x / 4) < n;
    }
    else
    {
        _399 = _391;
    }
    if (_399)
    {
        int r = (n - 1) - (point.x / 4);
        uint _412;
        if (r == 0)
        {
            _412 = v % 10u;
        }
        else
        {
            uint _421;
            if (r == 1)
            {
                _421 = 10u;
            }
            else
            {
                uint _428;
                if (r == 2)
                {
                    _428 = 10u;
                }
                else
                {
                    _428 = (r == 3) ? 100u : 1000u;
                }
                _421 = (v / _428) % 10u;
            }
            _412 = _421;
        }
        uint g = _412;
        if (((_452[g] >> uint(14 - ((point.y * 3) + spvSMod(point.x, 4)))) & 1u) != 0u)
        {
            return float3(1.0, 1.0, 0.4000000059604644775390625);
        }
    }
    return c * 0.25;
}

static inline __attribute__((always_inline))
float3 Crosshair(thread const float3& c, thread const int& index, constant Constants& constants, thread float4& gl_FragCoord, constant Ramp& ramp)
{
    float4 rect = ramp.crosshair_rects[index];
    uint4 style = ramp.crosshair_styles[index];
    if (style.x == 0u)
    {
        return c;
    }
    int2 size = int2(round(float2(1.0) / constants._inverse));
    float2 first = float2(constants.displacement) + (rect.xy * float2(size));
    float2 extent = rect.zw * float2(size);
    bool _536 = any(gl_FragCoord.xy < first);
    bool _547;
    if (!_536)
    {
        _547 = any(gl_FragCoord.xy >= (first + extent));
    }
    else
    {
        _547 = _536;
    }
    if (_547)
    {
        return c;
    }
    int e = max(2, int(extent.y / 240.0));
    int2 middle = int2(round(first + (extent * 0.5)));
    float3 fill = _583[min(style.z, 6u)];
    if (style.y == 1u)
    {
        int2 d = abs(int2(gl_FragCoord.xy) - middle);
        int arm = int(fast::max(float(e), 2.0) * 0.89999997615814208984375);
        int along = max(d.x, d.y);
        int across = min(d.x, d.y);
        bool _624 = along == 0;
        bool _632;
        if (!_624)
        {
            _632 = along > (arm + 1);
        }
        else
        {
            _632 = _624;
        }
        if (_632 || (across > 1))
        {
            return c;
        }
        return select(float3(0.0), fill, bool3((along <= arm) && (across == 0)));
    }
    float radius = fast::max(float(e) * 0.25, 0.75);
    float dist = length(gl_FragCoord.xy - (float2(middle) + float2(0.5)));
    float3 color = mix(c, float3(0.0), float3(fast::clamp((radius + 1.5) - dist, 0.0, 1.0) * 0.85000002384185791015625));
    return mix(color, fill, float3(fast::clamp((radius + 0.5) - dist, 0.0, 1.0)));
}

static inline __attribute__((always_inline))
uint NoteCode(thread const int& i, constant Constants& constants)
{
    return (constants.note[i / 5] >> uint(6 * spvSMod(i, 5))) & 63u;
}

static inline __attribute__((always_inline))
float3 ScreenNote(thread const float3& c, constant Constants& constants, thread float4& gl_FragCoord)
{
    int e = int(constants.counter.w);
    int y = int(gl_FragCoord.y) - 8;
    bool _715 = y < 0;
    bool _723;
    if (!_715)
    {
        _723 = y >= (7 * e);
    }
    else
    {
        _723 = _715;
    }
    if (_723)
    {
        return c;
    }
    int n = 0;
    for (;;)
    {
        bool _736 = n < 20;
        bool _743;
        if (_736)
        {
            int param = n;
            _743 = NoteCode(param, constants) != 0u;
        }
        else
        {
            _743 = _736;
        }
        if (_743)
        {
            n++;
            continue;
        }
        else
        {
            break;
        }
    }
    int width = ((n * 4) + 1) * e;
    int right = constants.displacement.x + int(round(1.0 / constants._inverse.x));
    int2 q = int2(int(gl_FragCoord.x) - ((right - 8) - width), y);
    bool _776 = q.x < 0;
    bool _784;
    if (!_776)
    {
        _784 = q.x >= width;
    }
    else
    {
        _784 = _776;
    }
    if (_784)
    {
        return c;
    }
    int2 point = (q / int2(e)) - int2(1);
    bool _797 = point.x >= 0;
    bool _803;
    if (_797)
    {
        _803 = point.y >= 0;
    }
    else
    {
        _803 = _797;
    }
    bool _809;
    if (_803)
    {
        _809 = point.y < 5;
    }
    else
    {
        _809 = _803;
    }
    bool _816;
    if (_809)
    {
        _816 = spvSMod(point.x, 4) < 3;
    }
    else
    {
        _816 = _809;
    }
    bool _824;
    if (_816)
    {
        _824 = (point.x / 4) < n;
    }
    else
    {
        _824 = _816;
    }
    if (_824)
    {
        int param_1 = point.x / 4;
        uint g = _860[min(NoteCode(param_1, constants), 42u)];
        if (((g >> uint(14 - ((point.y * 3) + spvSMod(point.x, 4)))) & 1u) != 0u)
        {
            return float3(1.0, 1.0, 0.4000000059604644775390625);
        }
    }
    return c * 0.25;
}

static inline __attribute__((always_inline))
uint BannerCode(thread const int& i, constant Constants& constants)
{
    return (constants.banner[i / 20][spvSMod(i, 20) / 5] >> uint(6 * spvSMod(i, 5))) & 63u;
}

static inline __attribute__((always_inline))
float3 ScreenBanner(thread const float3& c, constant Constants& constants, thread float4& gl_FragCoord)
{
    int e = int(constants.banner_info.x);
    int y = int(gl_FragCoord.y) - 8;
    bool _917 = y < 0;
    bool _925;
    if (!_917)
    {
        _925 = y >= (7 * e);
    }
    else
    {
        _925 = _917;
    }
    if (_925)
    {
        return c;
    }
    int n = 0;
    for (;;)
    {
        bool _938 = n < 40;
        bool _945;
        if (_938)
        {
            int param = n;
            _945 = BannerCode(param, constants) != 0u;
        }
        else
        {
            _945 = _938;
        }
        if (_945)
        {
            n++;
            continue;
        }
        else
        {
            break;
        }
    }
    int width = ((n * 4) + 1) * e;
    int middle = constants.displacement.x + int(round(0.5 / constants._inverse.x));
    int2 q = int2(int(gl_FragCoord.x) - (middle - (width / 2)), y);
    bool _976 = q.x < 0;
    bool _984;
    if (!_976)
    {
        _984 = q.x >= width;
    }
    else
    {
        _984 = _976;
    }
    if (_984)
    {
        return c;
    }
    int2 point = (q / int2(e)) - int2(1);
    bool _997 = point.x >= 0;
    bool _1003;
    if (_997)
    {
        _1003 = point.y >= 0;
    }
    else
    {
        _1003 = _997;
    }
    bool _1009;
    if (_1003)
    {
        _1009 = point.y < 5;
    }
    else
    {
        _1009 = _1003;
    }
    bool _1016;
    if (_1009)
    {
        _1016 = spvSMod(point.x, 4) < 3;
    }
    else
    {
        _1016 = _1009;
    }
    bool _1024;
    if (_1016)
    {
        _1024 = (point.x / 4) < n;
    }
    else
    {
        _1024 = _1016;
    }
    if (_1024)
    {
        int param_1 = point.x / 4;
        uint g = _860[min(BannerCode(param_1, constants), 42u)];
        if (((g >> uint(14 - ((point.y * 3) + spvSMod(point.x, 4)))) & 1u) != 0u)
        {
            return float3(1.0, 0.550000011920928955078125, 0.3499999940395355224609375);
        }
    }
    return c * 0.25;
}

fragment bo2zm_output_ramp_out bo2zm_output_ramp(constant Constants& constants [[buffer(0)]], constant Ramp& ramp [[buffer(2)]], texture2d<float> source [[texture(0)]], sampler tex_sampler [[sampler(1)]], float4 gl_FragCoord [[position]])
{
    bo2zm_output_ramp_out out = {};
    int2 pixel = int2(gl_FragCoord.xy) - constants.displacement;
    float3 texel;
    if (kFxaa)
    {
        float2 param = (float2(uint2(pixel)) + float2(0.5)) * constants._inverse;
        texel = Fxaa(param, source, tex_sampler, constants);
    }
    else
    {
        if (kExact)
        {
            texel = source.read(uint2(pixel), 0).xyz;
        }
        else
        {
            texel = source.sample(tex_sampler, ((float2(uint2(pixel)) + float2(0.5)) * constants._inverse), level(0.0)).xyz;
        }
    }
    uint3 i = uint3((texel * 255.0) + float3(0.5));
    float3 c = float3(ramp.inputs[i.x].x, ramp.inputs[i.y].y, ramp.inputs[i.z].z);
    if (kGrading)
    {
        float luma = dot(fast::max(c, float3(0.0)), float3(0.2125999927520751953125, 0.715200006961822509765625, 0.072200000286102294921875));
        c = mix(float3(luma), c, float3(ramp.blend.x));
        float saturated = fast::max(c.x, fast::max(c.y, c.z)) - fast::min(c.x, fast::min(c.y, c.z));
        c = mix(float3(luma), c, float3(1.0 + (ramp.blend.y * (1.0 - fast::clamp(saturated, 0.0, 1.0)))));
        c = powr(fast::max(c, float3(9.9999997473787516355514526367188e-05)), float3(ramp.blend.z));
        float2 uv = (float2(uint2(pixel)) + float2(0.5)) * constants._inverse;
        float v = ramp.effects.x;
        c *= ((((1.0 - (v * fast::max(0.0, 1.0 - (uv.y / 0.4199999868869781494140625)))) * (1.0 - (v * fast::max(0.0, 1.0 - ((1.0 - uv.y) / 0.4199999868869781494140625))))) * (1.0 - (v * fast::max(0.0, 1.0 - (uv.x / 0.4199999868869781494140625))))) * (1.0 - (v * fast::max(0.0, 1.0 - ((1.0 - uv.x) / 0.4199999868869781494140625)))));
        if ((uint(pixel.y) % 3u) == 0u)
        {
            c *= (1.0 - (ramp.effects.y * 0.699999988079071044921875));
        }
        c = fast::clamp(c, float3(0.0), float3(1.0));
    }
    if ((constants.counter.z & 1u) != 0u)
    {
        float3 param_1 = c;
        c = Counter(param_1, constants, gl_FragCoord);
    }
    if ((constants.counter.z & 2u) != 0u)
    {
        for (int view = 0; view < 4; view++)
        {
            float3 param_2 = c;
            int param_3 = view;
            c = Crosshair(param_2, param_3, constants, gl_FragCoord, ramp);
        }
    }
    if (constants.counter.w != 0u)
    {
        float3 param_4 = c;
        c = ScreenNote(param_4, constants, gl_FragCoord);
    }
    if (constants.banner_info.x != 0u)
    {
        float3 param_5 = c;
        c = ScreenBanner(param_5, constants, gl_FragCoord);
    }
    out.color = float4(c, 1.0);
    return out;
}

)msl";
