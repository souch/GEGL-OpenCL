static const char* invert_gamma_cl_source = 
"__kernel void cl_invert_gamma(__global const float4 *in,       \n"
"                              __global       float4 *out)       \n"
"{        \n"
"  int gid = get_global_id(0);       \n"
"         \n"
"  float4 in_v = in[gid];       \n"
"  float4 out_v;       \n"
"  out_v.xyz = 1.0 - in_v.xyz;       \n"
"  out_v.w = in_v.w;       \n"
"  out[gid] = out_v;       \n"
"}       \n"
;

