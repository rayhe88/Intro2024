/****************************************************
  This code is ported to SYCLfrom the original 
  Marching Cubes implementation in the Nvidia CUDA SDK
  https://github.com/NVIDIA/cuda-samples/tree/master/Samples/5_Domain_Specific/marchingCubes
*****************************************************/


/*
  Marching cubes

  This sample extracts a geometric isosurface from a volume dataset using
  the marching cubes algorithm. It uses the scan (prefix sum) function from
  the Thrust library to perform stream compaction.  Similar techniques can
  be used for other problems that require a variable-sized output per
  thread.

  For more information on marching cubes see:
  http://local.wasp.uwa.edu.au/~pbourke/geometry/polygonise/
  http://en.wikipedia.org/wiki/Marching_cubes

  Volume data courtesy:
  http://www9.informatik.uni-erlangen.de/External/vollib/

  For more information on the Thrust library
  http://code.google.com/p/thrust/

  The algorithm consists of several stages:

  1. Execute "classifyVoxel" kernel
  This evaluates the volume at the corners of each voxel and computes the
  number of vertices each voxel will generate.
  It is executed using one thread per voxel.
  It writes two arrays - voxelOccupied and voxelVertices to global memory.
  voxelOccupied is a flag indicating if the voxel is non-empty.

  2. Scan "voxelOccupied" array (using Thrust scan)
  Read back the total number of occupied voxels from GPU to CPU.
  This is the sum of the last value of the exclusive scan and the last
  input value.

  3. Execute "compactVoxels" kernel
  This compacts the voxelOccupied array to get rid of empty voxels.
  This allows us to run the complex "generateTriangles" kernel on only
  the occupied voxels.

  4. Scan voxelVertices array
  This gives the start address for the vertex data for each voxel.
  We read back the total number of vertices generated from GPU to CPU.

  Note that by using a custom scan function we could combine the above two
  scan operations above into a single operation.

  5. Execute "generateTriangles" kernel
  This runs only on the occupied voxels.
  It looks up the field values again and generates the triangle data,
  using the results of the scan to write the output to the correct addresses.
  The marching cubes look-up tables are stored in 1D textures.

  6. Render geometry
  Using number of vertices from readback.
*/

// includes
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
//#include "helper_math.h"
#include "helper_string.h"

#include <sycl/sycl.hpp>
//#include <dpct/dpct.hpp>

#include "defines.h"

extern "C" void launch_classifyVoxel(sycl::queue &q, sycl::range<3> globalRange,
                                     uint *voxelVerts, uint *voxelOccupied, uchar *volume,
                                     sycl::uint3 gridSize, sycl::uint3 gridSizeShift,
                                     sycl::uint3 gridSizeMask, uint numVoxels,
                                     sycl::float3 voxelSize, float isoValue);

extern "C" void launch_compactVoxels(sycl::queue &q, sycl::range<3> globalRange,
                                     uint *compactedVoxelArray, uint *voxelOccupied,
                                     uint *voxelOccupiedScan, uint numVoxels);

extern "C" void launch_generateTriangles(sycl::queue &q, sycl::range<3> globalRange,
                                         sycl::float4 *pos, sycl::float4 *norm,
                                         uint *compactedVoxelArray,uint *numVertsScanned,
                                         sycl::uint3 gridSize, sycl::uint3 gridSizeShift,
                                         sycl::uint3 gridSizeMask, sycl::float3 voxelSize,
                                         float isoValue, uint activeVoxels, uint maxVerts);

extern "C" void allocateTextures(sycl::queue &q, uint **d_edgeTable, uint **d_triTable,
                                 uint **d_numVertsTable);
extern "C" void createVolumeTexture(uchar *d_volume, size_t buffSize);
extern "C" void destroyAllTextureObjects();
extern "C" void ThrustScanWrapper(unsigned int *output, unsigned int *input,
                                  unsigned int numElements);

const char *volumeFilename = "Bucky.raw";

sycl::uint3 gridSizeLog2 = sycl::uint3(5, 5, 5);
sycl::uint3 gridSizeShift;
sycl::uint3 gridSize;
sycl::uint3 gridSizeMask;

sycl::float3 voxelSize;
uint numVoxels = 0;
uint maxVerts = 0;
uint activeVoxels = 0;
uint totalVerts = 0;

float isoValue = 0.2f;
float dIsoValue = 0.005f;

sycl::float4 *d_pos = nullptr, *d_normal = nullptr;

uchar *d_volume = nullptr;
uint *d_voxelVerts = nullptr;
uint *d_voxelVertsScan = nullptr;
uint *d_voxelOccupied = nullptr;
uint *d_voxelOccupiedScan = nullptr;
uint *d_compVoxelArray = nullptr;

// tables
uint *d_numVertsTable = nullptr;
uint *d_edgeTable = nullptr;
uint *d_triTable = nullptr;

bool g_bValidate = false;

int *pArgc = nullptr;
char **pArgv = nullptr;

// forward declarations
void runAutoTest(int argc, char **argv);
void initMC(int argc, char **argv);
void computeIsosurface();
void dumpFile(void *dData, int data_bytes, const char *file_name);

template <class T>
void dumpBuffer(T *d_buffer, int nelements, int size_element);
void cleanup();

//void mainMenu(int i);

#define EPSILON 5.0f
#define THRESHOLD 0.30f

////////////////////////////////////////////////////////////////////////////////
// Load raw data from disk
////////////////////////////////////////////////////////////////////////////////
uchar *loadRawFile(char *filename, int size) {
  FILE *fp = fopen(filename, "rb");

  if (!fp) {
    fprintf(stderr, "Error opening file '%s'\n", filename);
    return 0;
  }

  uchar *data = (uchar *)malloc(size);
  size_t read = fread(data, 1, size, fp);
  fclose(fp);

  printf("Read '%s', %d bytes\n", filename, (int)read);

  return data;
}

void dumpFile(void *dData, int data_bytes, const char *file_name) {
  void *hData = malloc(data_bytes);
  sycl::queue q;
  q.memcpy(hData, dData, data_bytes).wait();
  FILE *fp = fopen(file_name, "wb");
  fwrite(hData, 1, data_bytes, fp);
  fclose(fp);
  free(hData);
}

template <class T>
void dumpBuffer(T *d_buffer, int nelements, int size_element) {
  uint bytes = nelements * size_element;
  T *h_buffer = (T *)malloc(bytes);
  sycl::queue q;
  q.memcpy(h_buffer, d_buffer, bytes).wait();

  for (int i = 0; i < nelements; i++) {
    printf("%d: %u\n", i, h_buffer[i]);
  }

  printf("\n");
  free(h_buffer);
}

void runAutoTest(int argc, char **argv) {
  sycl::queue q;

  // Initialize CUDA buffers for Marching Cubes
  initMC(argc, argv);

  computeIsosurface();

  /*

  char *ref_file = nullptr;
  getCmdLineArgumentString(argc, (const char **)argv, "file", &ref_file);

  enum DUMP_TYPE { DUMP_POS = 0, DUMP_NORMAL, DUMP_VOXEL };
  int dump_option = getCmdLineArgumentInt(argc, (const char **)argv, "dump");

  bool bTestResult = true;

  switch (dump_option) {
    case DUMP_POS:
      dumpFile((void *)d_pos, sizeof(float4) * maxVerts,
               "marchCube_posArray.bin");
      bTestResult = sdkCompareBin2BinFloat(
          "marchCube_posArray.bin", "posArray.bin",
          maxVerts * sizeof(float) * 4, EPSILON, THRESHOLD, argv[0]);
      break;

    case DUMP_NORMAL:
      dumpFile((void *)d_normal, sizeof(float4) * maxVerts,
               "marchCube_normalArray.bin");
      bTestResult = sdkCompareBin2BinFloat(
          "marchCube_normalArray.bin", "normalArray.bin",
          maxVerts * sizeof(float) * 4, EPSILON, THRESHOLD, argv[0]);
      break;

    case DUMP_VOXEL:
      dumpFile((void *)d_compVoxelArray, sizeof(uint) * numVoxels,
               "marchCube_compVoxelArray.bin");
      bTestResult = sdkCompareBin2BinFloat(
          "marchCube_compVoxelArray.bin", "compVoxelArray.bin",
          numVoxels * sizeof(uint), EPSILON, THRESHOLD, argv[0]);
      break;

    default:
      printf("Invalid validation flag!\n");
      printf("-dump=0 <check position>\n");
      printf("-dump=1 <check normal>\n");
      printf("-dump=2 <check voxel>\n");
      exit(EXIT_SUCCESS);
  }

  exit(bTestResult ? EXIT_SUCCESS : EXIT_FAILURE);

  */
}

////////////////////////////////////////////////////////////////////////////////
// Program main
////////////////////////////////////////////////////////////////////////////////
int main(int argc, char **argv) {
  pArgc = &argc;
  pArgv = argv;

  printf("[%s] - Starting...\n", argv[0]);

  if (checkCmdLineFlag(argc, (const char **)argv, "file") &&
      checkCmdLineFlag(argc, (const char **)argv, "dump")) {
    g_bValidate = true;
    runAutoTest(argc, argv);
  } else {
    runAutoTest(argc, argv);
  }

  exit(EXIT_SUCCESS);
}

////////////////////////////////////////////////////////////////////////////////
// initialize marching cubes
////////////////////////////////////////////////////////////////////////////////
void initMC(int argc, char **argv) {
  printf("Starting `initMC`\n");
  sycl::queue q;
  // parse command line arguments
  int n;

  if (checkCmdLineFlag(argc, (const char **)argv, "grid")) {
    n = getCmdLineArgumentInt(argc, (const char **)argv, "grid");
    gridSizeLog2.x() = gridSizeLog2.y() = gridSizeLog2.z() = n;
  }

  if (checkCmdLineFlag(argc, (const char **)argv, "gridx")) {
    n = getCmdLineArgumentInt(argc, (const char **)argv, "gridx");
    gridSizeLog2.x() = n;
  }

  if (checkCmdLineFlag(argc, (const char **)argv, "gridx")) {
    n = getCmdLineArgumentInt(argc, (const char **)argv, "gridx");
    gridSizeLog2.y() = n;
  }

  if (checkCmdLineFlag(argc, (const char **)argv, "gridz")) {
    n = getCmdLineArgumentInt(argc, (const char **)argv, "gridz");
    gridSizeLog2.z() = n;
  }

  char *filename;

  if (getCmdLineArgumentString(argc, (const char **)argv, "file", &filename)) {
    volumeFilename = filename;
  }

  gridSize =
      sycl::uint3(1 << gridSizeLog2.x(), 1 << gridSizeLog2.y(), 1 << gridSizeLog2.z());
  gridSizeMask = sycl::uint3(gridSize.x() - 1, gridSize.y() - 1, gridSize.z() - 1);
  gridSizeShift =
      sycl::uint3(0, gridSizeLog2.x(), gridSizeLog2.x() + gridSizeLog2.y());

  numVoxels = gridSize.x() * gridSize.y() * gridSize.z();
  voxelSize =
      sycl::float3(2.0f / gridSize.x(), 2.0f / gridSize.y(), 2.0f / gridSize.z());
  maxVerts = gridSize.x() * gridSize.y() * 100;

  printf("grid: %d x %d x %d = %d voxels\n", gridSize.x(), gridSize.y(), gridSize.z(),
         numVoxels);
  printf("max verts = %d\n", maxVerts);

#if SAMPLE_VOLUME
  // load volume data
  printf("Loading volume data\n");
  char *path = sdkFindFilePath(volumeFilename, argv[0]);

  if (path == nullptr) {
    fprintf(stderr, "Error finding file '%s'\n", volumeFilename);

    exit(EXIT_FAILURE);
  }
  printf("Setting grid size\n");

  int size = gridSize.x() * gridSize.y() * gridSize.z() * sizeof(uchar);
  uchar *volume = loadRawFile(path, size);

  printf("Setting device memory\n");
  d_volume = static_cast<uchar *>(sycl::malloc_device(size, q));
  q.memcpy(d_volume, volume, size).wait();
  free(volume);

  printf("Starting `createVolumeTexture`\n");
  createVolumeTexture(d_volume, size);
  
  printf("Finished loading volume data\n");
#endif

  if (g_bValidate) {
    d_pos = static_cast<sycl::float4 *>(sycl::malloc_device(maxVerts * sizeof(float) * 4, q));
    d_normal = static_cast<sycl::float4 *>(sycl::malloc_device(maxVerts * sizeof(float) * 4, q));
  }

  // allocate textures
  allocateTextures(q, &d_edgeTable, &d_triTable, &d_numVertsTable);

  // allocate device memory
  unsigned int memSize = sizeof(uint) * numVoxels;
  d_voxelVerts = static_cast<uint *>(sycl::malloc_device(memSize, q));
  d_voxelVertsScan = static_cast<uint *>(sycl::malloc_device(memSize, q));
  d_voxelOccupied = static_cast<uint *>(sycl::malloc_device(memSize, q));
  d_voxelOccupiedScan = static_cast<uint *>(sycl::malloc_device(memSize, q));
  d_compVoxelArray = static_cast<uint *>(sycl::malloc_device(memSize, q));

  printf("Finished `initMC`\n");
}

void cleanup() {
  sycl::queue q;
  if (g_bValidate) {
          sycl::free(d_pos, q);
          sycl::free(d_normal, q);
  }

  destroyAllTextureObjects();
  sycl::free(d_edgeTable, q);
  sycl::free(d_triTable, q);
  sycl::free(d_numVertsTable, q);
  sycl::free(d_voxelVerts, q);
  sycl::free(d_voxelVertsScan, q);
  sycl::free(d_voxelOccupied, q);
  sycl::free(d_voxelOccupiedScan, q);
  sycl::free(d_compVoxelArray, q);

  if (d_volume) {
          sycl::free(d_volume, q);
  }
}

#define DEBUG_BUFFERS 0

////////////////////////////////////////////////////////////////////////////////
//! Run the **SYCL** part of the computation
////////////////////////////////////////////////////////////////////////////////
void computeIsosurface() {
  sycl::queue q;
  int maxThreadsPerBlock = 1024;
  int threads = std::min(128, maxThreadsPerBlock);
  int numBlocks = (numVoxels + threads - 1) / threads;

  numBlocks = std::min(numBlocks, 65535);
  
  // used w/ nd range
  //sycl::range<3> grid(numBlocks, 1, 1);
  //sycl::range<3> threads_range(threads, 1, 1);

  sycl::range<3> globalRange(numBlocks, 1, threads);

  // get around maximum grid size of 65535 in each dimension
  //if (grid[0] > 65535) {
    //grid[1] = grid[0] / 32768;
    //grid[0] = 32768;
  //}
  printf("Starting `launch_classifyVoxel`\n");
  // calculate number of vertices need per voxel
  launch_classifyVoxel(q, globalRange, d_voxelVerts, d_voxelOccupied, d_volume,
                       gridSize, gridSizeShift, gridSizeMask, numVoxels,
                       voxelSize, isoValue);
  printf("Finished `launch_classifyVoxel`\n");
#if DEBUG_BUFFERS
  printf("voxelVerts:\n");
  dumpBuffer(d_voxelVerts, numVoxels, sizeof(uint));
#endif

#if SKIP_EMPTY_VOXELS
  // scan voxel occupied array
  ThrustScanWrapper(d_voxelOccupiedScan, d_voxelOccupied, numVoxels);

#if DEBUG_BUFFERS
  printf("voxelOccupiedScan:\n");
  dumpBuffer(d_voxelOccupiedScan, numVoxels, sizeof(uint));
#endif

  // read back values to calculate total number of non-empty voxels
  // since we are using an exclusive scan, the total is the last value of
  // the scan result plus the last value in the input array
  {
    uint lastElement, lastScanElement;
    q.memcpy(&lastElement, d_voxelOccupied + numVoxels - 1, sizeof(uint)).wait();
    q.memcpy(&lastScanElement, d_voxelOccupiedScan + numVoxels - 1, sizeof(uint)).wait();
    activeVoxels = lastElement + lastScanElement;
  }

  if (activeVoxels == 0) {
    // return if there are no full voxels
    totalVerts = 0;
    return;
  }

  // compact voxel index array
  launch_compactVoxels(q, globalRange, d_compVoxelArray, d_voxelOccupied,
                       d_voxelOccupiedScan, numVoxels);
  q.wait();

#endif  // SKIP_EMPTY_VOXELS

  // scan voxel vertex count array
  ThrustScanWrapper(d_voxelVertsScan, d_voxelVerts, numVoxels);

#if DEBUG_BUFFERS
  printf("voxelVertsScan:\n");
  dumpBuffer(d_voxelVertsScan, numVoxels, sizeof(uint));
#endif

  // readback total number of vertices
  {
    uint lastElement, lastScanElement;
    q.memcpy(&lastElement, d_voxelVerts + numVoxels - 1, sizeof(uint)).wait();
    q.memcpy(&lastScanElement, d_voxelVertsScan + numVoxels - 1, sizeof(uint)).wait();
    totalVerts = lastElement + lastScanElement;
  }

  // generate triangles, writing to vertex buffers
#if SKIP_EMPTY_VOXELS
  sycl::range<3> globalRange2((int)ceil(activeVoxels / (float)NTHREADS), 1, NTHREADS);
#else
  sycl::range<3> globalRange2((int)ceil(numVoxels / (float)NTHREADS), 1, NTHREADS);
#endif

  while (globalRange2[0] > 65535) {
    globalRange2[0] /= 2;
    globalRange2[1] *= 2;
  }

  launch_generateTriangles(q, globalRange2, d_pos, d_normal, d_compVoxelArray,
                           d_voxelVertsScan, gridSize, gridSizeShift,
                           gridSizeMask, voxelSize, isoValue, activeVoxels,
                           maxVerts);
  q.wait();
}

