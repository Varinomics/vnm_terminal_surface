#version 440

layout(location = 0) in vec4 fragment_color;
layout(location = 0) out vec4 output_color;
layout(location = 0, index = 1) out vec4 output_blend_factor;

void main()
{
    output_color = fragment_color;
    output_blend_factor = vec4(fragment_color.a);
}
