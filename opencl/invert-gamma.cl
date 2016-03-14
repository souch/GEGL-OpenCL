// v1
__kernel void cl_invert_gamma(__global const float4 *in,
                              __global       float4 *out)
{ 
  int gid = get_global_id(0);
  
  float4 in_v = in[gid];
  float4 out_v;
  out_v.xyz = 1.0 - in_v.xyz;
  out_v.w = in_v.w;
  out[gid] = out_v;
}

