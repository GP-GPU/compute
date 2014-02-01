//---------------------------------------------------------------------------//
// Copyright (c) 2013-2014 Kyle Lutz <kyle.r.lutz@gmail.com>
//
// Distributed under the Boost Software License, Version 1.0
// See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt
//
// See http://kylelutz.github.com/compute for more information.
//---------------------------------------------------------------------------//

#include <iostream>
#include <algorithm>

#include <GL/glx.h>

#include <vtkActor.h>
#include <vtkCamera.h>
#include <vtkgl.h>
#include <vtkInteractorStyleSwitch.h>
#include <vtkMapper.h>
#include <vtkObjectFactory.h>
#include <vtkOpenGLExtensionManager.h>
#include <vtkOpenGLRenderWindow.h>
#include <vtkProperty.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkSmartPointer.h>

#include <boost/compute/source.hpp>
#include <boost/compute/system.hpp>
#include <boost/compute/algorithm/iota.hpp>
#include <boost/compute/interop/opengl.hpp>
#include <boost/compute/interop/vtk.hpp>

namespace compute = boost::compute;

// tesselates a sphere with radius, phi_slices, and theta_slices. returns
// a shared opencl/opengl buffer containing the vertex data.
compute::opengl_buffer tesselate_sphere(float radius,
                                        size_t phi_slices,
                                        size_t theta_slices,
                                        compute::command_queue &queue)
{
    const compute::context &context = queue.get_context();

    const size_t vertex_count = phi_slices * theta_slices;

    // create opengl buffer
    GLuint vbo;
    vtkgl::GenBuffersARB(1, &vbo);
    vtkgl::BindBufferARB(vtkgl::ARRAY_BUFFER, vbo);
    vtkgl::BufferDataARB(vtkgl::ARRAY_BUFFER,
                         sizeof(float) * 4 * vertex_count,
                         NULL,
                         vtkgl::STREAM_DRAW);
    vtkgl::BindBufferARB(vtkgl::ARRAY_BUFFER, 0);

    // create shared opengl/opencl buffer
    compute::opengl_buffer vertex_buffer(context, vbo);

    // tesselate_sphere kernel source
    const char source[] = BOOST_COMPUTE_STRINGIZE_SOURCE(
        __kernel void tesselate_sphere(float radius,
                                       uint phi_slices,
                                       uint theta_slices,
                                       __global float4 *vertex_buffer)
        {
            const uint phi_i = get_global_id(0);
            const uint theta_i = get_global_id(1);

            const float phi = phi_i * 2.f * M_PI_F / phi_slices;
            const float theta = theta_i * 2.f * M_PI_F / theta_slices;

            float4 v;
            v.x = radius * cos(theta) * cos(phi);
            v.y = radius * cos(theta) * sin(phi);
            v.z = radius * sin(theta);
            v.w = 1.f;

            vertex_buffer[phi_i*phi_slices+theta_i] = v;
        }
    );

    // build tesselate_sphere program
    compute::program program =
        compute::program::create_with_source(source, context);
    program.build();

    // setup tesselate_sphere kernel
    compute::kernel kernel(program, "tesselate_sphere");
    kernel.set_arg<compute::float_>(0, radius);
    kernel.set_arg<compute::uint_>(1, phi_slices);
    kernel.set_arg<compute::uint_>(2, theta_slices);
    kernel.set_arg(3, vertex_buffer);

    // acqurire buffer so that it is accessible to OpenCL
    compute::opengl_enqueue_acquire_buffer(vertex_buffer, queue);

    // execute tesselate_sphere kernel
    size_t offset[2] = { 0, 0 };
    size_t work_size[2] = { phi_slices, theta_slices };
    size_t group_size[2] = { 1, 1 };
    queue.enqueue_nd_range_kernel(kernel, 2, offset, work_size, group_size);

    // release buffer so that it is accessible to OpenGL
    compute::opengl_enqueue_release_buffer(vertex_buffer, queue);

    return vertex_buffer;
}

// simple vtkMapper subclass to render the tesselated sphere on the gpu.
class gpu_sphere_mapper : public vtkMapper
{
public:
    vtkTypeMacro(gpu_sphere_mapper, vtkMapper)

    static gpu_sphere_mapper* New()
    {
        return new gpu_sphere_mapper;
    }

    void Render(vtkRenderer *renderer, vtkActor *actor)
    {
        if(!m_initialized){
            Initialize(renderer, actor);
            m_initialized = true;
        }

        if(!m_tesselated){
            m_vertex_count = m_phi_slices * m_theta_slices;

            // tesselate sphere
            m_vertex_buffer = tesselate_sphere(
                m_radius, m_phi_slices, m_theta_slices, m_command_queue
            );

            // set tesselated flag to true
            m_tesselated = true;
        }

        // draw sphere
        glEnableClientState(GL_VERTEX_ARRAY);
        vtkgl::BindBufferARB(vtkgl::ARRAY_BUFFER, m_vertex_buffer.get_opengl_object());
        glVertexPointer(4, GL_FLOAT, sizeof(float)*4, 0);
        glDrawArrays(GL_POINTS, 0, m_vertex_count);
    }

    void Initialize(vtkRenderer *renderer, vtkActor *actor)
    {
        // initialize opengl extensions
        vtkOpenGLExtensionManager *extensions =
            static_cast<vtkOpenGLRenderWindow *>(renderer->GetRenderWindow())
                ->GetExtensionManager();
        extensions->LoadExtension("GL_ARB_vertex_buffer_object");

        // initialize opencl/opengl shared context
        compute::device device = compute::system::default_device();
        std::cout << "device: " << device.name() << std::endl;
        if(!device.supports_extension("cl_khr_gl_sharing")){
            std::cerr << "error: "
                      << "gpu device: " << device.name()
                      << " does not support OpenGL sharing"
                      << std::endl;
             return;
        }

        // create context for the gpu device
        cl_context_properties properties[] = {
            CL_GL_CONTEXT_KHR, (cl_context_properties) glXGetCurrentContext(),
            CL_GLX_DISPLAY_KHR, (cl_context_properties) glXGetCurrentDisplay(),
            0
        };
        m_context = compute::context(device, properties);

        // create command queue for the gpu device
        m_command_queue = compute::command_queue(m_context, device);
    }

    double* GetBounds()
    {
        static double bounds[6];
        bounds[0] = -m_radius; bounds[1] = m_radius;
        bounds[2] = -m_radius; bounds[3] = m_radius;
        bounds[4] = -m_radius; bounds[5] = m_radius;
        return bounds;
    }

protected:
    gpu_sphere_mapper()
    {
        m_radius = 5.0f;
        m_phi_slices = 100;
        m_theta_slices = 100;
        m_initialized = false;
        m_tesselated = false;
    }

private:
    float m_radius;
    int m_phi_slices;
    int m_theta_slices;
    int m_vertex_count;
    bool m_initialized;
    bool m_tesselated;
    compute::context m_context;
    compute::command_queue m_command_queue;
    compute::opengl_buffer m_vertex_buffer;
};

int main(int argc, char *argv[])
{
    // create gpu sphere mapper
    vtkSmartPointer<gpu_sphere_mapper> mapper =
        vtkSmartPointer<gpu_sphere_mapper>::New();

    // create actor for gpu sphere mapper
    vtkSmartPointer<vtkActor> actor =
        vtkSmartPointer<vtkActor>::New();
    actor->GetProperty()->LightingOff();
    actor->GetProperty()->SetInterpolationToFlat();
    actor->SetMapper(mapper);

    // create render window
    vtkSmartPointer<vtkRenderer> renderer =
        vtkSmartPointer<vtkRenderer>::New();
    renderer->SetBackground(.1, .2, .31);
    vtkSmartPointer<vtkRenderWindow> renderWindow =
        vtkSmartPointer<vtkRenderWindow>::New();
    renderWindow->SetSize(800, 600);
    renderWindow->AddRenderer(renderer);
    vtkSmartPointer<vtkRenderWindowInteractor> renderWindowInteractor =
        vtkSmartPointer<vtkRenderWindowInteractor>::New();
    vtkInteractorStyleSwitch *interactorStyle =
        vtkInteractorStyleSwitch::SafeDownCast(
            renderWindowInteractor->GetInteractorStyle()
        );
    interactorStyle->SetCurrentStyleToTrackballCamera();
    renderWindowInteractor->SetRenderWindow(renderWindow);
    renderer->AddActor(actor);

    // render
    renderer->ResetCamera();
    vtkCamera *camera = renderer->GetActiveCamera();
    camera->Elevation(-90.0);
    renderWindowInteractor->Initialize();
    renderWindow->Render();
    renderWindowInteractor->Start();

    return 0;
}