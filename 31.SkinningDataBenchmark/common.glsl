#ifdef __cplusplus
#define uint uint32_t
#endif
#define MAX_OBJ_CNT 3000
#define MAX_BONE_CNT 37
#define MAT_MAX_CNT uint (MAX_OBJ_CNT * MAX_BONE_CNT)
#define BONE_VEC_MAX_CNT uint (MAT_MAX_CNT * 4)
#define NORM_VEC_MAX_CNT uint (MAT_MAX_CNT * 3)
#define BONE_COMP_MAX_CNT uint (MAT_MAX_CNT * 16)
#define NORM_COMP_MAX_CNT uint (MAT_MAX_CNT * 9)