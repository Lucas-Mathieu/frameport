#version 450
// alpha_blend_vk.c: with blending (one minus destination alpha, zero) and only alpha written, the image's alpha
// becomes 1 - alpha.
layout(location=0) out vec4 outputColor;
void main(){outputColor=vec4(0,0,0,1);}
