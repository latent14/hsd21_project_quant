#include "fpga_api.h"
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <cstring>


#define min(x, y) (((x) < (y)) ? (x) : (y))

FPGA::FPGA(off_t data_addr, off_t output_addr, int m_size, int v_size)
{
  m_size_ = m_size;
  v_size_ = v_size;
  data_size_ = (m_size_ + 1) * v_size_; // fpga bram data size

  qvec_ = new char[v_size_];
  qmat_ = new char[m_size_*v_size_];

  m1_size_ = v_size * v_size;
  m2_size_ = v_size * v_size;
  data_size_M = (v_size_+v_size_)*v_size_;
  
  qm1_ = new char[v_size_*v_size_];
  qm2_ = new char[v_size_*v_size_];
  
  qout_ = new int[m_size_];
  qout_M = new int[v_size_*v_size_];

  output_ = new unsigned int[m_size_]; // use output_ as tempolar output
  output_M = new unsigned int[v_size_*v_size_]; // use output_M as tempolar output

  data_ = new float[data_size_];
  data_M = new float[data_size_M];

  fd_ = open("/dev/mem", O_RDWR);

  qdata_ = new int[data_size_];
  qdata_M = static_cast<int *>(mmap(NULL, data_size_M, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, data_addr));
  
  output_ = static_cast<unsigned int *>(mmap(NULL, sizeof(unsigned int), PROT_READ | PROT_WRITE, MAP_SHARED, fd_, output_addr));
  num_block_call_ = 0;
}

FPGA::~FPGA()
{
  munmap(qdata_M, data_size_);
  munmap(output_, sizeof(unsigned int));
  close(fd_);

  delete[] output_;
  delete[] data_;
  delete[] qvec_;
  delete[] qmat_;
  delete[] qout_;
}

int *FPGA::qmatrix(void)
{
  return qdata_ + v_size_;
}

int *FPGA::qvector(void)
{
  return qdata_;
}

int *FPGA::qmatrix_M1(void)
{
  return qdata_M;
}

int *FPGA::qmatrix_M2(void)
{
  return qdata_M + m1_size_;
}

void FPGA::reset(void)
{
  num_block_call_ = 0;
}

int FPGA::num_block_call(void)
{
  return num_block_call_;
}

void quantize(const float* input, int* quantized, int num_input, int bits_min, int bits_max, int offset, float scale)
{
  for(int i = 0; i < num_input; i++)
  {
    float q = ceil(input[i]/scale) + offset;
    if(q > bits_max) quantized[i] = bits_max - offset;
    else if(q < bits_min) quantized[i] = bits_min - offset;
    else quantized[i] = q - offset;
  }
}

void dequantize(int* quantized, float* output, int num_output, int offset, float scale)
{
  for(int i = 0; i < num_output; i++)
  {
    output[i] = (float)(quantized[i] * scale);
  }
}

const int *__attribute__((optimize("O0"))) FPGA::qblockMM(Compute* comp)
{
  num_block_call_ += 1;

  // fpga version
  *output_ = 0x5555;
  while (*output_ == 0x5555)
    ;

  return qdata_;
}

const float* FPGA::blockMM(Compute* comp)
{
  num_block_call_ += 1;

  // cpu version
  int* m1 = this->qmatrix_M1();
  int* m2 = this->qmatrix_M2();
  float* out  = reinterpret_cast<float*>(output_M);  

  if(comp->quantized)
  {
    float* weight = this->matrix_M1();
    float* act = this->matrix_M2();

    char act_bits_min = 0;
    char act_bits_max = (1<<(comp->act_bits-1))-1;

    float act_scale = (comp->act_max - comp->act_min) / (act_bits_max - act_bits_min); // TODO calculate the scale factor
    char act_offset = act_bits_min - ceil(comp->act_min / act_scale); // TODO calculate the zero-offset
    quantize(act, m2, m2_size_, act_bits_min, act_bits_max, act_offset, act_scale); // TODO complete quantize function

    char weight_bits_min = 0;
    char weight_bits_max = (1<<(comp->weight_bits-1))-1;

    float weight_scale = (comp->weight_max - comp->weight_min) / (weight_bits_max - weight_bits_min); // TODO calculate the scale factor
    char weight_offset = weight_bits_min - ceil(comp->weight_min / weight_scale); // TODO calculate the zero-offset
    quantize(weight, m1, m1_size_, weight_bits_min, weight_bits_max, weight_offset, weight_scale); // TODO complete quantize function

    for(int i = 0; i < v_size_; ++i)
    {
      for(int j = 0; j < v_size_; ++j){    
        qout_M[v_size_*i+j] = 0;
        for(int k = 0; k < v_size_; ++k){
          qout_M[v_size_*i+j] += m1[v_size_*i+k] * m2[v_size_*k + j];
        }
      }
    }
    dequantize(qout_M, out, v_size_ * v_size_, 0, act_scale*weight_scale); // TODO complete dequantize function

  }
  else{
    for(int i = 0; i < v_size_; ++i)
    {
      for(int j = 0; j < v_size_; ++j){    
        out[v_size_*i+j] = 0;
        for(int k = 0; k < v_size_; ++k){
          out[v_size_*i+j] += m1[v_size_*i+k] * m2[v_size_*k + j];
        }
      }
    }
  }

  for(int i = 0; i < m1_size_; ++i)
    data_M[i] = out[i];

  return data_M;    
}

const float *FPGA::blockMV(Compute* comp)
{
  num_block_call_ += 1;

  // cpu version
  int *vec = this->qvector();
  int *mat = this->qmatrix();
  float *out = reinterpret_cast<float *>(output_);

  if(comp->quantized)
  {
    float* weight = this->matrix_M1();
    float* act = this->matrix_M2();

    char act_bits_min = 0;
    char act_bits_max = (1<<(comp->act_bits-1))-1;

    float act_scale = (comp->act_max - comp->act_min) / (act_bits_max - act_bits_min); // TODO calculate the scale factor
    char act_offset = act_bits_min - ceil(comp -> act_min / act_scale); // TODO calculate the zero-offset
    quantize(act, vec, v_size_, act_bits_min, act_bits_max, act_offset, act_scale); // TODO complete quantize function

    char weight_bits_min = 0;
    char weight_bits_max = (1<<(comp->weight_bits-1))-1;

    float weight_scale = (comp->weight_max - comp->weight_min) / (weight_bits_max - weight_bits_min); // TODO calculate the scale factor
    char weight_offset = weight_bits_min - ceil(comp->weight_min / weight_scale); // TODO calculate the zero-offset
    quantize(weight, mat, m_size_ * v_size_, weight_bits_min, weight_bits_max, weight_offset, weight_scale); // TODO complete quantize function

    for (int i = 0; i < m_size_; ++i)
    {
      qout_[i] = 0;
      for (int j = 0; j < v_size_; ++j)
        qout_[i] += (qvec_[j]-act_offset) * (qmat_[v_size_ * i + j]-weight_offset);
    }

    dequantize(qout_, out, m_size_, 0, act_scale*weight_scale);
  }
  else
  {
    for (int i = 0; i < m_size_; ++i)
    {
      out[i] = 0;
      for (int j = 0; j < v_size_; ++j)
        out[i] += vec[j] * mat[v_size_ * i + j];
    }
  }

  for (int i = 0; i < m_size_; ++i)
    data_[i] = out[i];

  return data_;
}

void FPGA::largeMM(const float* weight_mat, const float* input_mat, float* output, int num_input, int num_output, int num_matrix2, Compute* comp)
{
  float* m1 = this->matrix_M1();
  float* m2 = this->matrix_M2();

  // 0) Initialize output vector		
  for(int i = 0; i < num_output*num_matrix2; ++i)
    output[i] = 0;

  for(int i = 0; i < num_output; i += v_size_)
  {
    for(int j = 0; j < num_input; j += v_size_)
    {			
      for(int k = 0; k < num_matrix2; k += v_size_)
      {
        // 0) Initialize input vector
        int block_row = min(v_size_, num_output-i);
        int block_col_1 = min(v_size_, num_input-j);
        int block_col_2 = min(v_size_, num_matrix2-k);

        for(int r1=0; r1<v_size_; r1++){
	  for(int c1=0; c1<v_size_; c1++){
	    if(r1<block_row && c1<block_col_1) m1[r1*v_size_ + c1] = weight_mat[(r1+i) * num_input+(c1 +j)];
	    else m1[r1*v_size_+c1] = 0;
	  }
	}

        // 2) Assign a m2
        // IMPLEMENT THIS
	for(int r1=0; r1<v_size_; r1++){
	  for(int c1=0; c1<v_size_; c1++){
	    if(r1 < block_col_1 && c1<block_col_2) m2[r1*v_size_+c1] = input_mat[(r1+j) * num_matrix2 + (c1+k)];
	    else m2[r1*v_size_+c1] = 0;
	  }
	}

        // 3) Call a function `blockMM() to execute Matrix matrix multiplication
        const int* ret = this->qblockMM(comp);

        // 4) Accumulate intermediate results
        for(int n = 0; n<block_row; ++n)
        {
          for(int m = 0; m<block_col_2; ++m)
          {
            output[(i + n) + (k + m)*num_output] += ret[n*v_size_ + m];
          }
        }
      }
    } 
  }
}

void FPGA::largeMV(const float *large_mat, const float *input, float *output, int num_input, int num_output, Compute* comp)
{
  float *vec = this->vector();
  float *mat = this->matrix();

  // 0) Initialize output vector
  for (int i = 0; i < num_output; ++i)
    output[i] = 0;

  for (int i = 0; i < num_output; i += m_size_)
  {
    for (int j = 0; j < num_input; j += v_size_)
    {
      // 0) Initialize input vector
      int block_row = min(m_size_, num_output - i);
      int block_col = min(v_size_, num_input - j);

      for(int k=0; k<v_size_; k++){
	if(k<block_col) vec[k] = input[j+k];
	else vec[k] = 0;
      }

      // 2) Assign a matrix
      for(int r1=0; r1<m_size_; r1++){
	for(int c1=0; c1<v_size_; c1++){
	  if(r1<block_row && c1<block_col) mat[r1*v_size_+c1] = large_mat[(r1+i) * num_input + (c1+j)];
	  else mat[r1*v_size_+c1] = 0;
	}
      }

      // 3) Call a function `blockMV() to execute MV multiplication
      const float* ret = this->blockMV(comp);

      // 4) Accumulate intermediate results
      for (int row = 0; row < block_row; ++row)
        output[i + row] += ret[row];
    }
  }
}

void FPGA::convLowering(const std::vector<std::vector<std::vector<std::vector<float>>>> &cnn_weights,
                        std::vector<std::vector<float>> &new_weights,
                        const std::vector<std::vector<std::vector<float>>> &inputs,
                        std::vector<std::vector<float>> &new_inputs)
{
  /*
   * Arguments:
   *
   * conv_weights: [conv_channel, input_channel, conv_height, conv_width]
   * new_weights: [?, ?]
   * inputs: [input_channel, input_height, input_width]
   * new_inputs: [?, ?]
   *
   */

  int conv_channel = cnn_weights.size();
  int input_channel = cnn_weights[0].size();
  int conv_height = cnn_weights[0][0].size();
  int conv_width = cnn_weights[0][0][0].size();
  //int input_channel = inputs.size();
  int input_height = inputs[0].size();
  int input_width = inputs[0][0].size();

  // IMPLEMENT THIS
  // For example,
  // new_weights[0][0] = cnn_weights[0][0][0][0];
  // new_inputs[0][0] = inputs[0][0][0];
  for(int i = 0; i < input_channel; i++){
      for(int j = 0; j < conv_height; j++){
            for(int k = 0; k < conv_width; k++){
		for(int l = 0; l < conv_channel; l++){
		  new_weights[l][i*conv_height*conv_width + j*conv_width + k] = cnn_weights[l][i][j][k];}

		for(int n = 0; n < input_height-conv_height+1; n++){
		  for(int m = 0; m < input_width-conv_width+1; m++){
			 new_inputs[i*conv_height*conv_width + j*conv_width + k][n * (input_width-conv_width+1) + m] = inputs[i][n+j][m+k];}}
								  	}
						      }
			        }
}
