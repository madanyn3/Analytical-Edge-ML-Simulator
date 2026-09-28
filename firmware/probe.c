#include "arm_nnfunctions.h"
#include "arm_nnsupportfunctions.h"
#if !defined(ARM_MATH_DSP) || defined(ARM_MATH_MVEI)
#error Wrong architecture path
#endif
#if defined(CMSIS_NN_USE_SINGLE_ROUNDING) || defined(CMSIS_NN_USE_REQUANTIZE_INLINE_ASSEMBLY)
#error Wrong rounding configuration
#endif
__attribute__((section(".tensor_input"),aligned(16))) int8_t input[5];
__attribute__((section(".tensor_output"),aligned(16))) int8_t output[3];
__attribute__((aligned(16))) const int8_t weights[15]={1,2,-1,0,2,-1,1,2,-2,1,2,0,-3,1,2};
__attribute__((aligned(16))) const int32_t bias[3]={2,-12,34};
volatile int result_status;
// Analysis entry only: no vector table, clock setup or board startup is supplied.
void probe_entry(void) {
    input[0]=-3;input[1]=-1;input[2]=0;input[3]=2;input[4]=4;
    const cmsis_nn_context ctx={0};
    const cmsis_nn_fc_params p={.input_offset=1,.filter_offset=0,.output_offset=-1,.activation={-4,6}};
    const cmsis_nn_per_tensor_quant_params q={1073741824,-1};
    const cmsis_nn_dims in={1,1,1,5},filter={5,1,1,3},out={1,1,1,3},bd={1,1,1,3};
    result_status=arm_fully_connected_s8(&ctx,&p,&q,&in,input,&filter,weights,&bd,bias,&out,output);
    for(;;){}
}
