include "land_block_inc.sh"
include "bc_compression_inc.sh"

texture src_tex;
float src_mip = 0;
float dst_mip = 0;

int src_face = -1;
interval src_face : src_single_face < 0, src_cube;


shader bc1_srgbwrite_compressor, bc1_compressor
{
  USE_BC1_COMPRESSION(ps)
  hlsl(ps) {
    uint4 compress(half4 texels[16], half4 min_color, half4 max_color);
  }
  COMMON_BC_SHADER(shader == bc1_compressor, shader == bc1_srgbwrite_compressor)
  hlsl(ps) {
    uint4 compress(half4 texels[16], half4 min_color, half4 max_color)
    {
      find_base_colors( texels, min_color, max_color );
      refine_rgb_base_colors( texels, min_color, max_color );
      return pack_bc1_block( texels, min_color, max_color );
    }
  }
}
