/* Copyright 2016 Stanford University
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "render.h"
#include "renderImage.h"
#include "legion_visualization.h"
#include "legion_c_util.h"
#include "image_reduction_mapper.h"
#include <unistd.h>
#include <math.h>

using namespace Legion;
using namespace LegionRuntime::Accessor;


#define _T {std::cout<<__FUNCTION__<<":"<<__LINE__<<std::endl;}


#ifdef __cplusplus
extern "C" {
#endif
  
  
  
  static map<int, Visualization::ImageReduction*> gImageCompositors;
  static MapperID gImageReductionMapperID = 0;
  static int gRenderTaskID = 0;
  static int gSaveImageTaskID = 0;
  static bool gIsMasterNode = false;
  static const unsigned gImageWidth = 1280;
  static const unsigned gImageHeight = 720;

  
#define SAVE_RENDER_DATA 0
  
#if SAVE_RENDER_DATA
  
  // SAVE_RENDER_DATA is to enable offline debugging
  
  static void saveFluidRenderData(Context ctx,
                                  HighLevelRuntime *runtime,
                                  const Task* task,
                                  PhysicalRegion& fluid,
                                  std::vector<legion_field_id_t> fluidFields) {
    static int frameNumber = 0;
    
    char filename[256] = "render.fluid.";
    sprintf(filename + strlen(filename), "%d.%lld", frameNumber++, task->get_unique_id());
    FILE *fluidOut = fopen(filename, "w");
    
    AccessorRO<FieldData, 3> rho(fluid, fluidFields[0]);
    AccessorRO<FieldData, 3> pressure(fluid, fluidFields[1]);
    AccessorRO<FieldData3, 3> velocity(fluid, fluidFields[2]);
    AccessorRO<FieldData3, 3> centerCoordinates(fluid, fluidFields[3]);
    AccessorRO<FieldData, 3> temperature(fluid, fluidFields[7]);
    
    IndexSpace indexSpace = fluid.get_logical_region().get_index_space();
    Domain domain = runtime->get_index_space_domain(ctx, indexSpace);
    Rect<3> rect = domain;
    
    for (PointInRectIterator<3> pir(rect); pir(); pir++) {
      fprintf(fluidOut, "%g\t%g\t%g\t%g\t%g\t%g\t%g\t%g\t%g\n",
              rho[*pir], pressure[*pir],
              velocity[*pir].x[0], velocity[*pir].x[1], velocity[*pir].x[2],
              centerCoordinates[*pir].x[0], centerCoordinates[*pir].x[1], centerCoordinates[*pir].x[2],
              temperature[*pir]);
    }
    
    fclose(fluidOut);
  }
  
  static void saveParticlesRenderData(Context ctx,
                                      HighLevelRuntime *runtime,
                                      const Task* task,
                                      PhysicalRegion& particles,
                                      std::vector<legion_field_id_t> particlesFields) {
    
    static int frameNumber = 0;
    char filename[256] = "render.particles.";
    sprintf(filename + strlen(filename), "%d.%lld", frameNumber++, task->get_unique_id());
    FILE *particlesOut = fopen(filename, "w");
    
    AccessorRO<long int, 1> id(particles, particlesFields[0]);
    AccessorRO<FieldData3, 1> particlesPosition(particles, particlesFields[2]);
    AccessorRO<FieldData, 1> particlesTemperature(particles, particlesFields[4]);
    AccessorRO<FieldData, 1> particlesDensity(particles, particlesFields[6]);
    
    IndexSpace indexSpace = particles.get_logical_region().get_index_space();
    Domain domain = runtime->get_index_space_domain(ctx, indexSpace);
    Rect<1> rect = domain;
    
    for (PointInRectIterator<1> pir(rect); pir(); pir++) {
      fprintf(particlesOut, "%ld\t%g\t%g\t%g\t%g\t%g\n",
              id[*pir], particlesPosition[*pir].x[0], particlesPosition[*pir].x[1], particlesPosition[*pir].x[2],
              particlesTemperature[*pir], particlesDensity[*pir]);
    }
    
    fclose(particlesOut);
  }
#endif
  
  
  // Render task is a single task launched from every cxx_render entry point
  
  static void render_task(const Task *task,
                          const std::vector<PhysicalRegion> &regions,
                          Context ctx, HighLevelRuntime *runtime) {
    char hostname[128];
    gethostname(hostname, sizeof(hostname));
    std::cout << "in render_task " << task->task_id << " " << task->get_unique_id() << " pid " << getpid() << " " << hostname << std::endl;
    
    // Get physical regions and fields
    
    PhysicalRegion fluid = regions[0];
    PhysicalRegion particles = regions[1];
    PhysicalRegion image = regions[2];
    
    std::vector<legion_field_id_t> fluidFields;
    fluid.get_fields(fluidFields);
    
    std::vector<legion_field_id_t> particlesFields;
    particles.get_fields(particlesFields);
    
    std::vector<legion_field_id_t> imageFields;
    image.get_fields(imageFields);
    
#if SAVE_RENDER_DATA
    saveFluidRenderData(ctx, runtime, task, fluid, fluidFields);
    saveParticlesRenderData(ctx, runtime, task, particles, particlesFields);
#else
    
    // Get task arguments
    
    char* argsPtr = (char*)task->args;
    ImageDescriptor* imageDescriptor = (ImageDescriptor*)(argsPtr);
    argsPtr += sizeof(ImageDescriptor);
    FieldData* lowerBound = (FieldData*)argsPtr;
    argsPtr += sizeof(FieldData) * 3;
    FieldData* upperBound = (FieldData*)argsPtr;
    argsPtr += sizeof(FieldData) * 3;
    int numParticlesToDraw = *((int*)argsPtr);
    argsPtr += sizeof(int);
    VisualizationField isosurfaceField = *(VisualizationField*)argsPtr;
    argsPtr += sizeof(VisualizationField);
    double isosurfaceValue = *(double*)argsPtr;
    argsPtr += sizeof(double);
    double* isosurfaceScale = (double*)argsPtr;
    argsPtr += 2 * sizeof(double);
    long int* particlesToDraw = (long int*)argsPtr;
    
    // Initialize the renderer and its graphics context
    
    OSMesaContext mesaCtx;
    GLubyte* rgbaBuffer;
    GLfloat* depthBuffer;
    
    renderInitialize(lowerBound, upperBound, mesaCtx, rgbaBuffer, depthBuffer);
    
    // Render an image
    
    AccessorRO<FieldData, 3> rho(fluid, fluidFields[0]);
    AccessorRO<FieldData, 3> pressure(fluid, fluidFields[1]);
    AccessorRO<FieldData3, 3> velocity(fluid, fluidFields[2]);
    AccessorRO<FieldData3, 3> centerCoordinates(fluid, fluidFields[3]);
    AccessorRO<FieldData, 3> temperature(fluid, fluidFields[7]);
    
    AccessorRO<long int, 1> id(particles, particlesFields[0]);
    AccessorRO<FieldData3, 1> particlesPosition(particles, particlesFields[2]);
    AccessorRO<FieldData, 1> particlesTemperature(particles, particlesFields[4]);
    AccessorRO<FieldData, 1> particlesDensity(particles, particlesFields[6]);
    
    IndexSpace particlesIndexSpace = particles.get_logical_region().get_index_space();
    Rect<1> particlesBounds = runtime->get_index_space_domain(ctx, particlesIndexSpace);
    long int numParticles = particlesBounds.hi[0] - particlesBounds.lo[0];
    
    IndexSpace indexSpace = fluid.get_logical_region().get_index_space();
    Rect<3> bounds = runtime->get_index_space_domain(ctx, indexSpace);
    long int num[3] = {
      bounds.hi[0] - bounds.lo[0] + 1,
      bounds.hi[1] - bounds.lo[1] + 1,
      bounds.hi[2] - bounds.lo[2] + 1
    };
    
    Rect<3> rect3 = runtime->get_index_space_domain(ctx, fluid.get_logical_region().get_index_space());
    Point<3> Z3 = rect3.lo;
    const FieldData* rhoP = rho.ptr(Z3);
    const FieldData* pressureP = pressure.ptr(Z3);
    const FieldData3* velocityP = velocity.ptr(Z3);
    const FieldData3* centerCoordinatesP = centerCoordinates.ptr(Z3);
    const FieldData* temperatureP = temperature.ptr(Z3);

    Rect<1> rect1 = runtime->get_index_space_domain(ctx, particles.get_logical_region().get_index_space());
    Point<1> Z1 = rect1.lo;
    const long int* idP = id.ptr(Z1);
    const FieldData3* particlesPositionP = particlesPosition.ptr(Z1);
    const FieldData* particlesTemperatureP = particlesTemperature.ptr(Z1);
    const FieldData* particlesDensityP = particlesDensity.ptr(Z1);
    
    renderImage(num[0], num[1], num[2], rhoP, pressureP, velocityP, centerCoordinatesP, temperatureP, lowerBound, upperBound, isosurfaceField, isosurfaceValue, isosurfaceScale,
                numParticles, idP, particlesPositionP, particlesTemperatureP, particlesDensityP,
                particlesToDraw, numParticlesToDraw);
    
    // write rendered pixels into source image region
    
    glReadPixels(0, 0, imageDescriptor->width, imageDescriptor->height, GL_DEPTH_COMPONENT, GL_FLOAT, depthBuffer);

    AccessorWO<ImageReduction::PixelField, 3> r(image, imageFields[0]);
    AccessorWO<ImageReduction::PixelField, 3> g(image, imageFields[1]);
    AccessorWO<ImageReduction::PixelField, 3> b(image, imageFields[2]);
    AccessorWO<ImageReduction::PixelField, 3> a(image, imageFields[3]);
    AccessorWO<ImageReduction::PixelField, 3> z(image, imageFields[4]);
    
#define USE_COMPOSITOR 1
#if USE_COMPOSITOR
    
    IndexSpace saveIndexSpace = image.get_logical_region().get_index_space();
    Rect<3> saveRect = runtime->get_index_space_domain(ctx, saveIndexSpace);
    
    int index = 0;
    for(PointInRectIterator<3> pir(saveRect); pir(); pir++) {
      r[*pir] = rgbaBuffer[index * 4] / 255.0;
      g[*pir] = rgbaBuffer[index * 4 + 1] / 255.0;
      b[*pir] = rgbaBuffer[index * 4 + 2] / 255.0;
      a[*pir] = rgbaBuffer[index * 4 + 3] / 255.0;
      z[*pir] = depthBuffer[index];
      index++;
    }
    
#else
    static int frameNumber = 0;
    char filename[128];
    sprintf(filename, "/scratch/snx3000/aheirich/singleNodeImage.%04d.tga", frameNumber++);
    write_targa(filename, rgbaBuffer, imageDescriptor->width, imageDescriptor->height);
    std::cout << "wrote image to " << filename << std::endl;
#endif
    
#define SAVE_INTERMEDIATE_IMAGES 0
#if SAVE_INTERMEDIATE_IMAGES
    {
      IndexSpace saveIndexSpace = image.get_logical_region().get_index_space();
      Rect<3> saveRect = runtime->get_index_space_domain(ctx, saveIndexSpace);
      PointInRectIterator<3> pir(saveRect);
      char filename[256];
      static int frameNumber = 0;
      sprintf(filename, "intermediateRGBA.%lld-%lld-%lld.%05d.tga", pir[0], pir[1], pir[2], frameNumber);
      FILE* f = fopen(filename, "w");
      if(f == nullptr) {
        std::cerr << "could not create file " << filename << std::endl;
        return;
      }
      fputc (0x00, f);  /* ID Length, 0 => No ID   */
      fputc (0x00, f);  /* Color Map Type, 0 => No color map included   */
      fputc (0x02, f);  /* Image Type, 2 => Uncompressed, True-color Image */
      fputc (0x00, f);  /* Next five bytes are about the color map entries */
      fputc (0x00, f);  /* 2 bytes Index, 2 bytes length, 1 byte size */
      fputc (0x00, f);
      fputc (0x00, f);
      fputc (0x00, f);
      fputc (0x00, f);  /* X-origin of Image */
      fputc (0x00, f);
      fputc (0x00, f);  /* Y-origin of Image */
      fputc (0x00, f);
      fputc (imageDescriptor->width & 0xff, f);      /* Image Width */
      fputc ((imageDescriptor->width>>8) & 0xff, f);
      fputc (imageDescriptor->height & 0xff, f);     /* Image Height   */
      fputc ((imageDescriptor->height>>8) & 0xff, f);
      fputc (0x18, f);     /* Pixel Depth, 0x18 => 24 Bits  */
      fputc (0x20, f);     /* Image Descriptor  */
      fclose(f);
      
      f = fopen(filename, "ab");  /* reopen in binary append mode */
//      IndexSpace saveIndexSpace = image.get_logical_region().get_index_space();
//      Rect<3> saveRect = runtime->get_index_space_domain(ctx, saveIndexSpace);
//      PointInRectIterator<3> pir(saveRect);
      ImageReduction::PixelField* BB = (ImageReduction::PixelField*)b.ptr(*pir);
      ImageReduction::PixelField* GG = (ImageReduction::PixelField*)g.ptr(*pir);
      ImageReduction::PixelField* RR = (ImageReduction::PixelField*)r.ptr(*pir);
      
      for(int y = imageDescriptor->height - 1; y >= 0; y--) {
        for(int x = 0; x < imageDescriptor->width; ++x) {
          int index = x + y * imageDescriptor->width;
          GLubyte b_ = BB[index] * 255;
          fputc(b_, f); /* write blue */
          GLubyte g_ = GG[index] * 255;
          fputc(g_, f); /* write green */
          GLubyte r_ = RR[index] * 255;
          fputc(r_, f);   /* write red */
        }
      }
      fclose(f);
      std::cout << "wrote image " << filename << std::endl;
      sprintf(filename, "intermediateZ.%lld-%lld-%lld.%05d.txt", pir[0], pir[1], pir[2], frameNumber);
      f = fopen(filename, "w");
      for(int y = imageDescriptor->height - 1; y >= 0; y--) {
        for(int x = 0; x < imageDescriptor->width; ++x) {
          int index = x + y * imageDescriptor->width;
          fprintf(f, "%g\n", depthBuffer[index]);
        }
      }
      fclose(f);
      std::cout << "wrote depth " << filename << std::endl;
      frameNumber++;
    }
#endif
    
    // Release graphics context and render buffers
    
    renderTerminate(mesaCtx, rgbaBuffer, depthBuffer);
#endif
    
  }
  
  // save_image_task is a single task launched on the master node only
  
  static int gFrameNumber = 0;
  
  static int save_image_task(const Task *task,
                             const std::vector<PhysicalRegion> &regions,
                             Context ctx, HighLevelRuntime *runtime) {
    ImageDescriptor* imageDescriptor = (ImageDescriptor*)(task->args);
    PhysicalRegion image = regions[0];
    std::vector<legion_field_id_t> imageFields;
    image.get_fields(imageFields);
    
    AccessorRO<ImageReduction::PixelField, 3> r(image, imageFields[0]);
    AccessorRO<ImageReduction::PixelField, 3> g(image, imageFields[1]);
    AccessorRO<ImageReduction::PixelField, 3> b(image, imageFields[2]);
    AccessorRO<ImageReduction::PixelField, 3> a(image, imageFields[3]);
    AccessorRO<ImageReduction::PixelField, 3> z(image, imageFields[4]);
    
    char filename[128];
    if(getenv("SCRATCH") != nullptr) {
      sprintf(filename, "%s/image.%05d.tga", getenv("SCRATCH"), gFrameNumber++);
    } else {
      sprintf(filename, "image.%05d.tga", gFrameNumber++);
    }
    FILE* f = fopen(filename, "w");
    if(f == nullptr) {
      std::cerr << "could not create file " << filename << std::endl;
      return -1;
    }
    fputc (0x00, f);  /* ID Length, 0 => No ID   */
    fputc (0x00, f);  /* Color Map Type, 0 => No color map included   */
    fputc (0x02, f);  /* Image Type, 2 => Uncompressed, True-color Image */
    fputc (0x00, f);  /* Next five bytes are about the color map entries */
    fputc (0x00, f);  /* 2 bytes Index, 2 bytes length, 1 byte size */
    fputc (0x00, f);
    fputc (0x00, f);
    fputc (0x00, f);
    fputc (0x00, f);  /* X-origin of Image */
    fputc (0x00, f);
    fputc (0x00, f);  /* Y-origin of Image */
    fputc (0x00, f);
    fputc (imageDescriptor->width & 0xff, f);      /* Image Width */
    fputc ((imageDescriptor->width>>8) & 0xff, f);
    fputc (imageDescriptor->height & 0xff, f);     /* Image Height   */
    fputc ((imageDescriptor->height>>8) & 0xff, f);
    fputc (0x18, f);     /* Pixel Depth, 0x18 => 24 Bits  */
    fputc (0x20, f);     /* Image Descriptor  */
    fclose(f);
    
    f = fopen(filename, "ab");  /* reopen in binary append mode */
    IndexSpace saveIndexSpace = image.get_logical_region().get_index_space();
    Rect<3> saveRect = runtime->get_index_space_domain(ctx, saveIndexSpace);
    PointInRectIterator<3> pir(saveRect);
    ImageReduction::PixelField* BB = (ImageReduction::PixelField*)b.ptr(*pir);
    ImageReduction::PixelField* GG = (ImageReduction::PixelField*)g.ptr(*pir);
    ImageReduction::PixelField* RR = (ImageReduction::PixelField*)r.ptr(*pir);
    
    for(int y = imageDescriptor->height - 1; y >= 0; y--) {
      for(int x = 0; x < imageDescriptor->width; ++x) {
        int index = x + y * imageDescriptor->width;
        GLubyte b_ = BB[index] * 255;
        fputc(b_, f); /* write blue */
        GLubyte g_ = GG[index] * 255;
        fputc(g_, f); /* write green */
        GLubyte r_ = RR[index] * 255;
        fputc(r_, f);   /* write red */
      }
    }
    fclose(f);
    std::cout << "wrote image " << filename << std::endl;
    return 0;
  }
  
  
  // Called from mapper before runtime has started
  void cxx_preinitialize(MapperID mapperID)
  {
    Visualization::ImageReduction::preinitializeBeforeRuntimeStarts();
    gIsMasterNode = true;
    
    // allocate physical regions contiguously in memory
    LayoutConstraintRegistrar layout_registrar(FieldSpace::NO_SPACE, "SOA layout");
    std::vector<DimensionKind> dim_order(4);
    dim_order[0] = DIM_X;
    dim_order[1] = DIM_Y;
    dim_order[2] = DIM_Z;
    dim_order[3] = DIM_F; // fields go last for SOA
    layout_registrar.add_constraint(OrderingConstraint(dim_order, true/*contig*/));
    LayoutConstraintID soa_layout_id = Runtime::preregister_layout(layout_registrar);
    
    // preregister render task
    gImageReductionMapperID = mapperID;
    gRenderTaskID = Legion::HighLevelRuntime::generate_static_task_id();
    TaskVariantRegistrar registrar(gRenderTaskID, "render_task");
    registrar.add_constraint(ProcessorConstraint(Processor::LOC_PROC))
    .add_layout_constraint_set(0/*index*/, soa_layout_id)
    .add_layout_constraint_set(1/*index*/, soa_layout_id)
    .add_layout_constraint_set(2/*index*/, soa_layout_id);
    
    Runtime::preregister_task_variant<render_task>(registrar, "render_task");
    
    // preregister save image task
    gSaveImageTaskID = Legion::HighLevelRuntime::generate_static_task_id();
    TaskVariantRegistrar registrarSaveImage(gSaveImageTaskID, "save_image_task");
    registrarSaveImage.add_constraint(ProcessorConstraint(Processor::LOC_PROC))
    .add_layout_constraint_set(0/*index*/, soa_layout_id);
    Runtime::preregister_task_variant<int, save_image_task>(registrarSaveImage, "save_image_task");
    
  }
  
  
  // this entry point is called once from the main task
  RegionPartition cxx_initialize(
                                    legion_runtime_t runtime_,
                                    legion_context_t ctx_,
                                    legion_mapper_id_t sampleId,
                                    legion_logical_partition_t fluidPartition_
                                    )
  {
    Runtime *runtime = CObjectWrapper::unwrap(runtime_);
    Context ctx = CObjectWrapper::unwrap(ctx_)->context();
    LogicalPartition fluidPartition = CObjectWrapper::unwrap(fluidPartition_);

    // Initialize an image compositor, or reuse an initialized one
    
    Visualization::ImageDescriptor imageDescriptor = { gImageWidth, gImageHeight, 1 };
    
    if(gImageCompositors.find(sampleId) == gImageCompositors.end()) {
      gImageCompositors[sampleId] = new Visualization::ImageReduction(fluidPartition, imageDescriptor, ctx, runtime, gImageReductionMapperID);
      ImageReductionMapper::registerRenderTaskName("render_task");
    }
    
    Visualization::ImageReduction* compositor = gImageCompositors[sampleId];
    RegionPartition result;
    result.indexSpace = CObjectWrapper::wrap(compositor->sourceIndexSpace());
    result.imageX = CObjectWrapper::wrap(compositor->sourceImage());
    result.colorSpace = CObjectWrapper::wrap(compositor->depthPartitionColorSpace());
    result.p_Image = CObjectWrapper::wrap(compositor->depthPartition());
    compositor->sourceImageFields(ctx,    result.imageFields);
    return result;
  }
  
  
  
  // this entry point is called from an index task for each tile
  void cxx_render(legion_runtime_t runtime_,
                  legion_context_t ctx_,
                  legion_mapper_id_t sampleId,
                  legion_physical_region_t* fluid_,
                  legion_field_id_t fluidFields[],
                  int numFluidFields,
                  legion_physical_region_t* particles_,
                  legion_field_id_t particlesFields[],
                  int numParticlesFields,
                  legion_physical_region_t* image_,
                  legion_field_id_t imageFields[],
                  int numImageFields,
                  legion_logical_partition_t fluidPartition_,
                  legion_logical_partition_t particlesPartition_,
                  int numParticlesToDraw,
                  int isosurfaceField,
                  double isosurfaceValue,
                  double isosurfaceScale[2],
                  legion_physical_region_t* particlesToDraw_,
                  FieldData lowerBound[3],
                  FieldData upperBound[3]
                  )
  {
    std::cout << __FUNCTION__ << " pid " << getpid() << std::endl;
    static bool firstTime = true;
    
    // Unwrap objects
    
    Runtime *runtime = CObjectWrapper::unwrap(runtime_);
    Context ctx = CObjectWrapper::unwrap(ctx_)->context();
    PhysicalRegion* fluid = CObjectWrapper::unwrap(fluid_[0]);
    LogicalPartition fluidPartition = CObjectWrapper::unwrap(fluidPartition_);
    PhysicalRegion* particles = CObjectWrapper::unwrap(particles_[0]);
    LogicalPartition particlesPartition = CObjectWrapper::unwrap(particlesPartition_);
    PhysicalRegion* particlesToDraw = CObjectWrapper::unwrap(particlesToDraw_[0]);
    PhysicalRegion* image = CObjectWrapper::unwrap(image_[0]);

    // Create projection functors
    
    Visualization::ImageReduction* compositor = gImageCompositors[sampleId];
    if(firstTime) {
      ImageReductionProjectionFunctor* functor0 = new ImageReductionProjectionFunctor(compositor->everywhereDomain(), fluidPartition);
      runtime->register_projection_functor(1, functor0);
      ImageReductionProjectionFunctor* functor1 = new ImageReductionProjectionFunctor(compositor->everywhereDomain(), particlesPartition);
      runtime->register_projection_functor(2, functor1);
      ImageReductionProjectionFunctor* functor2 = new ImageReductionProjectionFunctor(compositor->everywhereDomain(), compositor->depthPartition());
      runtime->register_projection_functor(3, functor2);
    }
    
    // Construct arguments to render task

    ArgumentMap argMap;
    ImageDescriptor imageDescriptor = compositor->imageDescriptor();
    size_t argSize = sizeof(imageDescriptor) + 6 * sizeof(FieldData) + sizeof(int) + sizeof(VisualizationField) + sizeof(double) + 2 * sizeof(double) +
      numParticlesToDraw * sizeof(long int);
    char args[argSize];
    char* argsPtr = args;
    memcpy(argsPtr, &imageDescriptor, sizeof(imageDescriptor));
    argsPtr += sizeof(imageDescriptor);
    memcpy(argsPtr, lowerBound, 3 * sizeof(FieldData));
    argsPtr += 3 * sizeof(FieldData);
    memcpy(argsPtr, upperBound, 3 * sizeof(FieldData));
    argsPtr += 3 * sizeof(FieldData);
    memcpy(argsPtr, &numParticlesToDraw, sizeof(int));
    argsPtr += sizeof(int);
    memcpy(argsPtr, &isosurfaceField, sizeof(VisualizationField));
    argsPtr += sizeof(VisualizationField);
    memcpy(argsPtr, &isosurfaceValue, sizeof(double));
    argsPtr += sizeof(double);
    memcpy(argsPtr, isosurfaceScale, 2 * sizeof(double));
    argsPtr += 2 * sizeof(double);
    
    // Copy particlesToDraw as a task argument
    
    std::vector<legion_field_id_t> particlesToDrawFields;
    particlesToDraw->get_fields(particlesToDrawFields);
    AccessorRO<long int, 1> particlesToDrawId(*particlesToDraw, particlesToDrawFields[0]);
 
    long int* longArgsPtr = (long int*)argsPtr;
    for(int i = 0; i < numParticlesToDraw; ++i) {
      Point<1> p = i;
      longArgsPtr[i] = particlesToDrawId[p];
    }

    // Setup the render task launch with region requirements
    
    IndexTaskLauncher renderLauncher(gRenderTaskID, compositor->everywhereDomain(), TaskArgument(args, argSize), argMap, Predicate::TRUE_PRED, false, gImageReductionMapperID);
    
    RegionRequirement req0(fluidPartition, 1, READ_ONLY, EXCLUSIVE, fluid->get_logical_region(), gImageReductionMapperID);
    for(int i = 0; i < numFluidFields; ++i) req0.add_field(fluidFields[i]);
    renderLauncher.add_region_requirement(req0);
    
    RegionRequirement req1(particlesPartition, 2, READ_ONLY, EXCLUSIVE, particles->get_logical_region(), gImageReductionMapperID);
    for(int i = 0; i < numParticlesFields; ++i) {
      req1.add_field(particlesFields[i]);
    }
    renderLauncher.add_region_requirement(req1);
    
    RegionRequirement req2(compositor->depthPartition(), 3, WRITE_DISCARD, EXCLUSIVE, image->get_logical_region(), gImageReductionMapperID);
    req2.add_field(Visualization::ImageReduction::FID_FIELD_R);
    req2.add_field(Visualization::ImageReduction::FID_FIELD_G);
    req2.add_field(Visualization::ImageReduction::FID_FIELD_B);
    req2.add_field(Visualization::ImageReduction::FID_FIELD_A);
    req2.add_field(Visualization::ImageReduction::FID_FIELD_Z);
    renderLauncher.add_region_requirement(req2);
    
    // Clear the mapping history so render_task will create it anew
    
    ImageReductionMapper::clearPlacement(fluidPartition);
    
    // Launch the render task
    
    FutureMap futures = runtime->execute_index_space(ctx, renderLauncher);
    futures.wait_all_results();
    firstTime = false;
  }
  
  
  
  // this entry point is called once from the Visualize task
  void cxx_reduce(legion_runtime_t runtime_,
                  legion_context_t ctx_,
                  legion_mapper_id_t sampleId
                  )
  {
#if !USE_COMPOSITOR
    return;
#endif
    std::cout << __FUNCTION__ << std::endl;
    
    Runtime *runtime = CObjectWrapper::unwrap(runtime_);
    Context ctx = CObjectWrapper::unwrap(ctx_)->context();
    
    Visualization::ImageReduction* compositor = gImageCompositors[sampleId];
    compositor->set_depth_func(GL_LESS);
    FutureMap futures = compositor->reduce_associative_commutative(ctx);
    futures.wait_all_results();
    
    // save the image
    ImageDescriptor imageDescriptor = compositor->imageDescriptor();
    TaskLauncher saveImageLauncher(gSaveImageTaskID, TaskArgument(&imageDescriptor, sizeof(ImageDescriptor)), Predicate::TRUE_PRED, gImageReductionMapperID);
    DomainPoint slice0 = Point<3>::ZEROES();
    LogicalRegion imageSlice0 = runtime->get_logical_subregion_by_color(compositor->depthPartition(), slice0);
    RegionRequirement req(imageSlice0, READ_ONLY, EXCLUSIVE, compositor->sourceImage());
    saveImageLauncher.add_region_requirement(req);
    saveImageLauncher.add_field(0/*idx*/, Visualization::ImageReduction::FID_FIELD_R);
    saveImageLauncher.add_field(0/*idx*/, Visualization::ImageReduction::FID_FIELD_G);
    saveImageLauncher.add_field(0/*idx*/, Visualization::ImageReduction::FID_FIELD_B);
    saveImageLauncher.add_field(0/*idx*/, Visualization::ImageReduction::FID_FIELD_A);
    saveImageLauncher.add_field(0/*idx*/, Visualization::ImageReduction::FID_FIELD_Z);
    Future result = runtime->execute_task(ctx, saveImageLauncher);
    result.get_result<int>();
  }
  
#ifdef __cplusplus
}
#endif


