#version 450
// A triangle that covers the whole viewport (alpha_blend_vk.c).
void main(){vec2 p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);gl_Position=vec4(p*2-1,0,1);}
