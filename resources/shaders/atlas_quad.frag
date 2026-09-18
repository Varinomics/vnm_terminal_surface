#version 440

layout(location = 0) in vec4 fragment_color;
layout(location = 0) out vec4 output_color;

layout(std140, binding = 1) uniform invert_block
{
    float invert_brightness;
};

vec3 invert_hsv_value(vec3 color)
{
    float value = max(max(color.r, color.g), color.b);
    return value <= 0.0
        ? vec3(1.0)
        : color * ((1.0 - value) / value);
}

vec4 render_color(vec4 color)
{
    if (invert_brightness <= 0.5 || color.a <= 0.0) {
        return color;
    }

    // Rectangle colors arrive premultiplied because this pass uses a
    // one/one-minus-source-alpha blend. Invert the straight color and
    // premultiply it again before returning to the normal blend path.
    return vec4(
        invert_hsv_value(color.rgb / color.a) * color.a,
        color.a);
}

void main()
{
    output_color = render_color(fragment_color);
}
