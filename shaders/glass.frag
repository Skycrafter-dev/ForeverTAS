#version 440
// Liquid glass: samples the live scene behind the panel, bends it through a
// rounded bevel, frosts the body, and lights the rim. The item is larger than
// the glass shape by a padding that carries the soft drop shadow.
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 itemSize;      // item size in px (glass + shadow padding)
    vec4 shapeRect;     // glass rectangle inside the item, px
    vec2 srcOrigin;     // item top-left in backdrop px
    vec2 srcScale;      // backdrop px per item px (UI zoom)
    vec2 srcSize;       // race view size in px
    float radius;       // corner radius, px
    float bevel;        // width of the curved rim, px
    float refraction;   // rim displacement, px
    float dispersion;   // chromatic split, fraction of displacement
    float frost;        // 0 = clear glass, 1 = fully blurred body
    float saturation;   // colour boost of what shows through
    vec4 tint;          // rgb + amount
    float lightAngle;   // radians, screen space (y down)
    float specular;     // rim highlight strength
    float shadowAlpha;
    float shadowRadius;
    float shadowOffset;
    float lift;         // 0..1 extra brightness while dragged / focused
    vec4 panelMap;      // panel layer's top-left and size in backdrop px
    float panelMix;     // 1 = the blurred panels show through (popups)
};

layout(binding = 1) uniform sampler2D sharpSource;
layout(binding = 2) uniform sampler2D blurSource;
// The panels, blurred, premultiplied: popups sit over them, not just over
// the race view.
layout(binding = 3) uniform sampler2D panelSource;

float sdRoundBox(vec2 p, vec2 b, float r)
{
    vec2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

float shape(vec2 px)
{
    vec2 c = shapeRect.xy + shapeRect.zw * 0.5;
    return sdRoundBox(px - c, shapeRect.zw * 0.5, radius);
}

float hash(vec2 p)
{
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

// The race view only covers the free area between docked panels. Outside it
// the glass sees the blurred view mirrored at its edges, like the ambient
// backdrop behind the panels.
vec2 mirrored(vec2 uv)
{
    vec2 m = mod(uv, 2.0);
    return mix(m, 2.0 - m, step(1.0, m));
}

vec3 sampleChannel(vec2 pos, float frostAmount, out bool inside)
{
    vec2 uv = pos / srcSize;
    inside = all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)));
    vec2 m = mirrored(uv);
    vec3 soft = texture(blurSource, m).rgb;
    vec3 col = inside ? mix(texture(sharpSource, m).rgb, soft, frostAmount) : soft;
    if (panelMix > 0.0) {
        vec4 panel = texture(panelSource, clamp((pos - panelMap.xy) / panelMap.zw, 0.0, 1.0));
        col = col * (1.0 - panel.a * panelMix) + panel.rgb * panelMix;
    }
    return col;
}

vec3 sampleScene(vec2 px, vec2 offset, float frostAmount)
{
    bool inside;
    vec3 r = sampleChannel(srcOrigin + (px + offset * (1.0 + dispersion)) * srcScale, frostAmount, inside);
    vec3 g = sampleChannel(srcOrigin + (px + offset) * srcScale, frostAmount, inside);
    vec3 b = sampleChannel(srcOrigin + (px + offset * (1.0 - dispersion)) * srcScale, frostAmount, inside);
    return vec3(r.r, g.g, b.b);
}

void main()
{
    vec2 px = qt_TexCoord0 * itemSize;
    float d = shape(px);

    // Shadow: only where the glass is not.
    float ds = shape(px - vec2(0.0, shadowOffset));
    float shadow = shadowAlpha * exp(-max(ds, 0.0) * max(ds, 0.0)
                                     / (0.5 * shadowRadius * shadowRadius));
    shadow *= smoothstep(-shadowRadius, 0.0, ds) * 0.5 + 0.5;

    float coverage = clamp(0.5 - d, 0.0, 1.0);
    if (coverage <= 0.0) {
        fragColor = vec4(0.0, 0.0, 0.0, shadow) * qt_Opacity;
        return;
    }

    // Outward normal of the rounded rectangle.
    vec2 e = vec2(0.75, 0.0);
    vec2 n = vec2(shape(px + e.xy) - shape(px - e.xy),
                  shape(px + e.yx) - shape(px - e.yx));
    n = n / max(length(n), 1e-4);

    // Circular bevel profile: steep at the edge, flat inside.
    float depth = -d;
    float t = clamp(depth / bevel, 0.0, 1.0);
    float x = 1.0 - t;
    float slope = clamp(x / sqrt(max(1.0 - x * x, 0.035)), 0.0, 5.0);
    vec2 offset = -n * slope * refraction;

    float rim = 1.0 - t;
    // Clear, bent ring at the very edge; frosted body inside it.
    float frostAmount = frost * (1.0 - 0.85 * pow(rim, 2.0));
    vec3 col = sampleScene(px, offset, frostAmount);

    // Saturation boost and tint.
    float luma = dot(col, vec3(0.2126, 0.7152, 0.0722));
    col = mix(vec3(luma), col, saturation);
    col = mix(col, tint.rgb, tint.a);
    col += lift * 0.035;

    // Lighting on the rim.
    vec2 L = vec2(cos(lightAngle), sin(lightAngle));
    float facing = dot(n, L);
    float edgeBand = exp(-depth / 1.6);
    float wideBand = rim * rim * rim;
    float key = pow(max(facing, 0.0), 2.0);
    float bounce = pow(max(-facing, 0.0), 2.0);
    vec3 spec = vec3(1.0) * specular * (key * (edgeBand * 1.0 + wideBand * 0.32)
                                        + bounce * (edgeBand * 0.5 + wideBand * 0.12));
    // Hairline all around so the shape stays readable over flat backdrops.
    spec += vec3(1.0) * edgeBand * 0.09;
    // Faint inner shade on the side away from the light: gives thickness.
    col *= 1.0 - bounce * wideBand * 0.18;
    // Soft sheen falling from the top edge.
    float top = clamp(1.0 - (px.y - shapeRect.y) / 120.0, 0.0, 1.0);
    spec += vec3(1.0) * top * top * 0.035;

    col += spec;
    col += (hash(px) - 0.5) * 0.012;

    float a = coverage;
    vec3 rgb = col * a;
    float alpha = a + shadow * (1.0 - a);
    fragColor = vec4(rgb, alpha) * qt_Opacity;
}
