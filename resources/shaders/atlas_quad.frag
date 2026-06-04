#version 440

layout(location = 0) in vec4 fragment_color;
layout(location = 0) out vec4 output_color;

void main()
{
    output_color = fragment_color;
}
