#version 440
// Fills the window around the race view with a blurred, mirrored and dimmed
// copy of it, so the glass panels and the gaps between them always have the
// scene behind them.
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 itemSize;      // window size in px
    vec2 viewOrigin;    // race view top-left in window px
    vec2 viewSize;      // race view size in px
    float dim;          // 0 = as is, 1 = black
};

layout(binding = 1) uniform sampler2D blurSource;

vec2 mirrored(vec2 uv)
{
    vec2 m = mod(uv, 2.0);
    return mix(m, 2.0 - m, step(1.0, m));
}

void main()
{
    vec2 px = qt_TexCoord0 * itemSize;
    vec2 uv = (px - viewOrigin) / max(viewSize, vec2(1.0));
    vec3 col = texture(blurSource, mirrored(uv)).rgb;
    // Darker the further from the race view.
    vec2 outside = max(max(-uv, uv - 1.0), vec2(0.0));
    float fade = clamp(length(outside * viewSize) / 900.0, 0.0, 1.0);
    col *= (1.0 - dim) * (1.0 - 0.35 * fade);
    fragColor = vec4(col, 1.0) * qt_Opacity;
}
