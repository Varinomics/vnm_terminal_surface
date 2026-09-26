#version 440

// Pairs with atlas_glyph.vert, whose outputs keep these names for GLSL
// targets that link varyings by name.
layout(location = 0) in vec2 fragment_uv;
layout(location = 1) in vec4 fragment_color;

layout(binding = 1) uniform sampler2D image_texture;

layout(location = 0) out vec4 output_color;

void main()
{
    // Image texels are premultiplied, so the inherited opacity in the
    // instance alpha scales every channel. Images are content, not theme, and
    // are never brightness-inverted.
    output_color = texture(image_texture, fragment_uv) * fragment_color.a;
}
