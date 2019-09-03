// ======================================================================== //
// BLOSPRAY - OSPRay as a Blender render engine                             //
// Paul Melis, SURFsara <paul.melis@surfsara.nl>                            //
// Render server                                                            //
// ======================================================================== //
// Copyright 2018-2019 SURFsara                                             //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#include <sys/time.h>
#include <sys/stat.h>
#include <stdint.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <iostream>
#include <string>
#include <unistd.h>
#include <dlfcn.h>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>

#include <ospray/ospray.h>
#include <ospray/ospray_testing/ospray_testing.h>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/string_cast.hpp>      // to_string()

#include "image.h"
#include "tcpsocket.h"
#include "json.hpp"
#include "blocking_queue.h"
#include "cool2warm.h"
#include "util.h"
#include "plugin.h"
#include "messages.pb.h"

using json = nlohmann::json;

const int   PORT = 5909;

OSPRenderer     renderer;
std::string     current_renderer_type;
OSPWorld        world;
OSPCamera       camera;
OSPFrameBuffer  framebuffer;

std::map<std::string, OSPRenderer>  renderers;
std::map<std::string, OSPMaterial>  materials;
std::vector<OSPInstance>            scene_instances;
std::vector<OSPLight>               scene_lights;

int             framebuffer_width=0, framebuffer_height=0;
bool            framebuffer_created = false;
bool            keep_framebuffer_files = getenv("BLOSPRAY_KEEP_FRAMEBUFFER_FILES") != nullptr;

OSPMaterial         default_material;               // XXX hack for now, renderer-type dependent

ImageSettings   image_settings;
RenderSettings  render_settings;
CameraSettings  camera_settings;

// Geometry buffers used during network receive

std::vector<float>      vertex_buffer;
std::vector<float>      normal_buffer;
std::vector<float>      vertex_color_buffer;
std::vector<uint32_t>   triangle_buffer;

// Plugin registry

typedef std::map<std::string, PluginDefinition> PluginDefinitionsMap;
typedef std::map<std::string, PluginState*>     PluginStateMap;

PluginDefinitionsMap    plugin_definitions;
PluginStateMap          plugin_state;

// Server-side data associated with blender Mesh Data that has a
// blospray plugin attached to it
struct PluginInstance
{
    PluginType      type;
    std::string     name;

    // XXX store hash of parameters that the instance was generated from

    // Plugin state contains OSPRay scene elements
    // XXX move properties out of PluginState?
    PluginState     *state;     // XXX store as object, not as pointer?
};

// A regular Blender Mesh
struct BlenderMesh
{
    std::string     name;
    json            parameters;     // XXX not sure we need this

    OSPGeometry     geometry;
};

// Server-side data associated with a blender Mesh Object

enum SceneObjectType
{
    SOT_MESH,           // Blender mesh    -> SOT_TRIANGLE_MESH
    SOT_GEOMETRY,       // OSPRay geometry
    SOT_VOLUME,
    SOT_SLICES,
    SOT_ISOSURFACES,
    SOT_SCENE,
    SOT_LIGHT           // In OSPRay these are actually stored on the renderer, not in the scene    // XXX not used?
};

enum SceneDataType
{
    SDT_PLUGIN,
    SDT_MESH
};

struct SceneObject
{
    SceneObjectType type;                   // XXX the type of scene objects actually depends on the type of data linked
    std::string     name;

    glm::mat4       object2world;
    //json            parameters;

    std::string     data_link;              // Name of linked scene data, may be ""
    //PluginInstance  *plugin_instance;       // May be NULL

    // Corresponding OSPRay elements in the scene (aka world)

    // Either a single OSPInstance in case of a geometry/volume/mesh,
    // or a list of instances of OSPGroup's (with their xforms) when
    // provided by a scene plugin
    std::vector<OSPInstance>    instances;          // Will have 1 item for type != SOT_SCENE
    OSPGroup                    group;              // Only for type in [SOT_GEOMETRY, SOT_VOLUME]
    OSPGeometricModel           geometric_model;    // Only for type == SOT_GEOMETRY
    OSPVolumetricModel          volumetric_model;   // Only for type == SOT_VOLUME
    OSPLight                    light;              // Only for type == SOT_LIGHT
};

// Top-level scene objects
std::map<std::string, SceneObject>      scene_objects;

// Mesh Data, either plugins or regular Blender meshes
typedef std::map<std::string, SceneDataType>    SceneDataTypeMap;

SceneDataTypeMap    scene_data_types;

std::map<std::string, PluginInstance*>  plugin_instances;
std::map<std::string, BlenderMesh*>     blender_meshes;

// Plugin handling

// If needed, loads plugin shared library and initializes plugin
// XXX perhaps this operation should have its own ...Result type
bool
ensure_plugin_is_loaded(GenerateFunctionResult &result, PluginDefinition &definition,
    const std::string& type, const std::string& name)
{
    if (type == "")
    {
        printf("No plugin type provided!\n");
        return false;
    }

    if (name == "")
    {
        printf("No plugin name provided!\n");
        return false;
    }

    const std::string internal_name = type + "_" + name;

    PluginDefinitionsMap::iterator it = plugin_definitions.find(internal_name);

    if (it == plugin_definitions.end())
    {
        // Plugin not loaded yet (or failed to load the previous attempt)

        printf("Plugin '%s' not loaded yet\n", internal_name.c_str());

        std::string plugin_file = internal_name + ".so";

        // Open plugin shared library

        printf("Loading plugin %s (%s)\n", internal_name.c_str(), plugin_file.c_str());

        void *plugin = dlopen(plugin_file.c_str(), RTLD_LAZY);

        if (!plugin)
        {
            result.set_success(false);
            result.set_message("Failed to open plugin");

            fprintf(stderr, "dlopen() error: %s\n", dlerror());
            return false;
        }

        dlerror();  // Clear previous error

        // Initialize plugin

        plugin_initialization_function *initialize = (plugin_initialization_function*) dlsym(plugin, "initialize");

        if (initialize == NULL)
        {
            result.set_success(false);
            result.set_message("Failed to get initialization function from plugin!");

            fprintf(stderr, "dlsym() error: %s\n", dlerror());

            dlclose(plugin);

            return false;
        }

        if (!initialize(&definition))
        {
            result.set_success(false);
            result.set_message("Plugin failed to initialize!");

            dlclose(plugin);

            return false;
        }

        plugin_definitions[internal_name] = definition;

        printf("Plugin parameters:\n");

        PluginParameter *p = definition.parameters;
        while (p->name)
        {
            printf("... [%s] type %d, length %d, flags 0x%02x - %s\n", p->name, p->type, p->length, p->flags, p->description);
            p++;
        }
    }
    else
        definition = plugin_definitions[internal_name];

    return true;
}

bool
check_plugin_parameters(GenerateFunctionResult& result, const PluginParameter *plugin_parameters, const json &actual_parameters)
{
    // We don't return false on the first error, but keep checking for any subsequent errors
    bool ok = true;

    for (const PluginParameter *pdef = plugin_parameters; pdef->name; pdef++)
    {
        const char *name = pdef->name;
        const int length = pdef->length;
        const ParameterType type = pdef->type;

        // XXX param might be optional in future
        if (actual_parameters.find(name) == actual_parameters.end())
        {
            printf("ERROR: Missing parameter '%s'!\n", name);
            ok = false;
            continue;
        }

        const json &value = actual_parameters[name];

        if (length > 1)
        {
            // Array value
            if (!value.is_array())
            {
                printf("ERROR: Expected array of length %d for parameter '%s'!\n", length, name);
                ok = false;
                continue;
            }

            // XXX check array items
        }
        else
        {
            // Scalar value
            if (!value.is_primitive())
            {
                printf("ERROR: Expected primitive value for parameter '%s', but found array of length %d!\n", name, value.size());
                ok = false;
                continue;
            }

            switch (type)
            {
            case PARAM_INT:
                if (!value.is_number_integer())
                {
                    printf("ERROR: Expected integer value for parameter '%s'!\n", name);
                    ok = false;
                    continue;
                }
                break;

            case PARAM_FLOAT:
                if (!value.is_number_float())
                {
                    printf("ERROR: Expected float value for parameter '%s'!\n", name);
                    ok = false;
                    continue;
                }
                break;

            //case PARAM_BOOL:
            case PARAM_STRING:
                if (!value.is_string())
                {
                    printf("ERROR: Expected string value for parameter '%s'!\n", name);
                    ok = false;
                    continue;
                }

            case PARAM_USER:
                break;
            }
        }
    }

    return ok;
}

void
prepare_renderers()
{
    OSPMaterial m;

    renderers["scivis"] = ospNewRenderer("scivis");

    m = materials["scivis"] = ospNewMaterial("scivis", "OBJMaterial");
        ospSetVec3f(m, "Kd", 0.8f, 0.8f, 0.8f);
    ospCommit(m);

    renderers["pathtracer"] = ospNewRenderer("pathtracer");

#if 1
    m = materials["pathtracer"] = ospNewMaterial("pathtracer", "OBJMaterial");
        ospSetVec3f(m, "Kd", 0.8f, 0.8f, 0.8f);
    ospCommit(m);
#else
    m = materials["pathtracer"] = ospNewMaterial("pathtracer", "MetallicPaint");
        ospSetVec3f(m, "baseColor", 0.8f, 0.3f, 0.3f);
        //ospSetFloat(m, "metallic", 0.5f);
    ospCommit(m);
#endif
}

OSPTransferFunction
create_transfer_function(const std::string& name, float minval, float maxval)
{
    printf("create_transfer_function('%s', %.6f, %.6f)\n", name.c_str(), minval, maxval);

	if (name == "jet")
	{
		osp_vec2f voxelRange = { minval, maxval };
		return ospTestingNewTransferFunction(voxelRange, "jet");
    }
    else if (name == "cool2warm")
    {
		// XXX should do this only once
	    float tf_colors[3*cool2warm_entries];
	    float tf_opacities[cool2warm_entries];

	    for (int i = 0; i < cool2warm_entries; i++)
	    {
	        tf_opacities[i]  = cool2warm[4*i+0];
	        tf_colors[3*i+0] = cool2warm[4*i+1];
	        tf_colors[3*i+1] = cool2warm[4*i+2];
	        tf_colors[3*i+2] = cool2warm[4*i+3];
	    }

    	OSPTransferFunction tf = ospNewTransferFunction("piecewise_linear");

        	ospSetVec2f(tf, "valueRange", minval, maxval);

        	OSPData color_data = ospNewData(cool2warm_entries, OSP_VEC3F, tf_colors);
        	ospSetData(tf, "color", color_data);        	

        	// XXX color and opacity can be decoupled?
        	OSPData opacity_data = ospNewData(cool2warm_entries, OSP_FLOAT, tf_opacities);
        	ospSetData(tf, "opacity", opacity_data);        	

    	ospCommit(tf);
        ospRelease(color_data);
        ospRelease(opacity_data);        

    	return tf;
	}

    return nullptr;
}

bool
handle_update_blender_mesh(TCPSocket *sock, const std::string& name)
{
    printf("BLENDER MESH '%s'\n", name.c_str());

    OSPGeometry geometry;
    BlenderMesh *blender_mesh;

    SceneDataTypeMap::iterator it = scene_data_types.find(name);
    if (it == scene_data_types.end())
    {
        // No previous mesh with this name
        printf("... Unseen name, creating new mesh\n");
        geometry = ospNewGeometry("triangles");
        scene_data_types[name] = SDT_MESH;
        blender_mesh = blender_meshes[name] = new BlenderMesh;
        blender_mesh->geometry = geometry;
    }
    else
    {
        // Have existing scene data with this name, check what it is
        SceneDataType type = it->second;

        if (type != SDT_MESH)
        {
            printf("WARNING: scene data '%s' is currently of type %d, overwriting with mesh!\n", name.c_str(), type);

            // XXX do the overwriting correctly ;-)    
            // erase existing entries

            geometry = ospNewGeometry("triangles");  
            blender_mesh = blender_meshes[name] = new BlenderMesh;
            blender_mesh->geometry = geometry;  
        }
        else
        {
            printf("WARNING: mesh '%s' already present, overwriting!\n", name.c_str());            
            blender_mesh = blender_meshes[name];
            geometry = blender_mesh->geometry;
        }
    }

    MeshData    mesh_data;
    OSPData     data;
    uint32_t    nv, nt, flags;    

    if (!receive_protobuf(sock, mesh_data))
        return false;

    nv = mesh_data.num_vertices();
    nt = mesh_data.num_triangles();
    flags = mesh_data.flags();

    printf("... %d vertices, %d triangles, flags 0x%08x\n", nv, nt, flags);

    vertex_buffer.reserve(nv*3);
    if (sock->recvall(&vertex_buffer[0], nv*3*sizeof(float)) == -1)
        return false;

    if (flags & MeshData::NORMALS)
    {
        printf("... Mesh has normals\n");
        normal_buffer.reserve(nv*3);
        if (sock->recvall(&normal_buffer[0], nv*3*sizeof(float)) == -1)
            return false;
    }

    if (flags & MeshData::VERTEX_COLORS)
    {
        printf("... Mesh has vertex colors\n");
        vertex_color_buffer.reserve(nv*4);
        if (sock->recvall(&vertex_color_buffer[0], nv*4*sizeof(float)) == -1)
            return false;
    }

    triangle_buffer.reserve(nt*3);
    if (sock->recvall(&triangle_buffer[0], nt*3*sizeof(uint32_t)) == -1)
        return false;

    data = ospNewData(nv, OSP_VEC3F, &vertex_buffer[0]);
    ospCommit(data);
    ospSetData(geometry, "vertex.position", data);
    ospRelease(data);

    if (flags & MeshData::NORMALS)
    {
        data = ospNewData(nv, OSP_VEC3F, &normal_buffer[0]);
        ospCommit(data);
        ospSetData(geometry, "vertex.normal", data);
        ospRelease(data);
    }

    if (flags & MeshData::VERTEX_COLORS)
    {
        data = ospNewData(nv, OSP_VEC4F, &vertex_color_buffer[0]);
        ospCommit(data);
        ospSetData(geometry, "vertex.color", data);
        ospRelease(data);
    }

    data = ospNewData(nt, OSP_VEC3I, &triangle_buffer[0]);
    ospCommit(data);
    ospSetData(geometry, "index", data);
    ospRelease(data);

    ospCommit(geometry);

    return true;
}

bool
handle_update_plugin_instance(TCPSocket *sock)
{
    UpdatePluginInstance    update;

    if (!receive_protobuf(sock, update))
        return false;

    //print_protobuf(update);

    const std::string& data_name = update.name();

    printf("PLUGIN INSTANCE [%s]\n", data_name.c_str());

    PluginInstance *plugin_instance;
    PluginState *state;    

    SceneDataTypeMap::iterator it = scene_data_types.find(data_name);

    if (it == scene_data_types.end())
    {
        // No previous plugin instance with this name
        printf("... Unseen name, creating new plugin instance\n");
                             
        plugin_state[data_name] = state = new PluginState; 
        state->renderer = current_renderer_type;   

        plugin_instances[data_name] = plugin_instance = new PluginInstance;  
        plugin_instance->state = state;   
    }
    else
    {
        // Have existing scene data with this name, check what it is
        SceneDataType type = it->second;

        if (type != SDT_PLUGIN)
        {
            printf("WARNING: scene data '%s' is currently of type %d, overwriting with plugin instance!\n", data_name.c_str(), type);

            // XXX erase existing entries            

            plugin_state[data_name] = state = new PluginState; 
            state->renderer = current_renderer_type;  

            plugin_instances[data_name] = plugin_instance = new PluginInstance;  
            plugin_instance->state = state;   
        }
        else
        {
            printf("WARNING: plugin instance '%s' already present, overwriting!\n", data_name.c_str());                        
            
            plugin_instance = plugin_instances[data_name];
            state = plugin_state[data_name];
        }
    }

    scene_data_types[data_name] = SDT_PLUGIN;

    std::string plugin_type;

    // XXX handle changing of plugin type
    switch (update.type())
    {
    case UpdatePluginInstance::GEOMETRY:
        plugin_type = "geometry";
        plugin_instance->type = PT_GEOMETRY;
        break;
    case UpdatePluginInstance::VOLUME:
        plugin_type = "volume";
        plugin_instance->type = PT_VOLUME;
        break;
    case UpdatePluginInstance::SCENE:
        plugin_type = "scene";
        plugin_instance->type = PT_SCENE;
        break;
    default:
        printf("... WARNING: unknown plugin instance type!\n");
        return false;
    }

    const std::string &plugin_name = update.plugin_name();

    printf("... plugin type: %s\n", plugin_type.c_str());
    printf("... plugin name: '%s'\n", plugin_name.c_str());

    const char *s_plugin_parameters = update.plugin_parameters().c_str();
    //printf("Received plugin parameters:\n%s\n", s_plugin_parameters);
    const json &plugin_parameters = json::parse(s_plugin_parameters);
    printf("... parameters:\n");
    printf("%s\n", plugin_parameters.dump(4).c_str());

    const char *s_custom_properties = update.custom_properties().c_str();
    //printf("Received custom properties:\n%s\n", s_custom_properties);
    const json &custom_properties = json::parse(s_custom_properties);
    printf("... custom properties:\n");
    printf("%s\n", custom_properties.dump(4).c_str());

    // Prepare result

    GenerateFunctionResult result;

    // By default all is well, we let the plugin signal something went wrong 
    result.set_success(true);

    // Find generate function

    PluginDefinition plugin_definition;

    if (!ensure_plugin_is_loaded(result, plugin_definition, plugin_type, plugin_name))
    {
        // Something went wrong...
        send_protobuf(sock, result);
        return false;
    }

    generate_function_t generate_function = plugin_definition.functions.generate_function;

    if (generate_function == NULL)
    {
        printf("Plugin returned NULL generate_function!\n");
        exit(-1);
    }

    // Check parameters passed to generate function

    if (!check_plugin_parameters(result, plugin_definition.parameters, plugin_parameters))
    {
        // Something went wrong...
        send_protobuf(sock, result);
        return false;
    }

    state->parameters = plugin_parameters;

    // Call generate function

    struct timeval t0, t1;

    printf("Calling generate function\n");
    gettimeofday(&t0, NULL);

    generate_function(result, state);

    gettimeofday(&t1, NULL);
    printf("Generate function executed in %.3fs\n", time_diff(t0, t1));
    
    if (!result.success())
    {
        printf("ERROR: generate function failed:\n");
        printf("... %s\n", result.message().c_str());
        send_protobuf(sock, result);
        return false;
    }

    // Handle any other business for this type of plugin
    // XXX set result.success to false?

    switch (update.type())
    {
    case UpdatePluginInstance::GEOMETRY:

        if (state->geometry == NULL)
        {
            send_protobuf(sock, result);

            printf("ERROR: geometry generate function did not set an OSPGeometry!\n");
            return false;
        }    

        break;

    case UpdatePluginInstance::VOLUME:

        if (state->volume == NULL)
        {
            send_protobuf(sock, result);

            printf("ERROR: volume generate function did not set an OSPVolume!\n");
            return false;
        }

        break;

    case UpdatePluginInstance::SCENE:

        if (state->group_instances.size() == 0)
            printf("WARNING: scene generate function returned 0 instances!\n");    

        break;
    }

    // Load function succeeded

    scene_data_types[data_name] = SDT_PLUGIN;

    send_protobuf(sock, result);

    return true;
}


bool
add_blender_mesh(const UpdateObject& update)
{
    const std::string& linked_data = update.data_link();

    printf("OBJECT '%s' (blender mesh)\n", update.name().c_str());    

    SceneDataTypeMap::iterator it = scene_data_types.find(linked_data);

    if (it == scene_data_types.end())
    {
        printf("--> '%s' | WARNING: no linked data found!\n", linked_data.c_str());
        return false;
    }
    else if (it->second != SDT_MESH)
    {
        printf("--> '%s' | WARNING: linked data is not of type 'mesh' but of type %d!\n", 
            linked_data.c_str(), it->second);
        return false;
    }
    else
        printf("--> '%s' (blender mesh data)\n", linked_data.c_str());

    BlenderMesh *blender_mesh = blender_meshes[linked_data];
    OSPGeometry geometry = blender_mesh->geometry;
    assert(geometry != NULL);

    glm::mat4   obj2world;
    float       affine_xform[12];

    object2world_from_protobuf(obj2world, update);
    affine3fv_from_mat4(affine_xform, obj2world);

    OSPGeometricModel model = ospNewGeometricModel(geometry);
        ospSetObject(model, "material", default_material);
    ospCommit(model);

    OSPData models = ospNewData(1, OSP_OBJECT, &model, 0);
    OSPGroup group = ospNewGroup();
        ospSetData(group, "geometry", models);
    ospCommit(group);
    ospRelease(model);
    ospRelease(models);

    OSPInstance instance = ospNewInstance(group);
        ospSetAffine3fv(instance, "xfm", affine_xform);
    ospCommit(instance);
    ospRelease(group);

    scene_instances.push_back(instance);

    return true;
}


bool
add_geometry_object(const UpdateObject& update)
{    
    printf("OBJECT [%s] (geometry)\n", update.name().c_str());    

    const std::string& linked_data = update.data_link();
    SceneDataTypeMap::iterator it = scene_data_types.find(linked_data);

    if (it == scene_data_types.end())
    {
        printf("--> '%s' | WARNING: no linked data found!\n", linked_data.c_str());
        return false;
    }
    else if (it->second != SDT_PLUGIN)
    {
        printf("--> '%s' | WARNING: linked data is not a plugin instance!\n", linked_data.c_str());
        return false;
    }
    else
        printf("--> '%s' (geometry XXX?)\n", linked_data.c_str());

    PluginInstance* plugin_instance = plugin_instances[linked_data];
    assert(plugin_instance->type == PT_GEOMETRY);
    PluginState *state = plugin_instance->state;

    OSPGeometry geometry = state->geometry;
    assert(geometry != NULL);

    glm::mat4   obj2world;
    float       affine_xform[12];

    object2world_from_protobuf(obj2world, update);
    affine3fv_from_mat4(affine_xform, obj2world);

    OSPGeometricModel model = ospNewGeometricModel(geometry);
        ospSetObject(model, "material", default_material);
    ospCommit(model);

    OSPData models = ospNewData(1, OSP_OBJECT, &model, 0);

    OSPGroup group = ospNewGroup();
        ospSetData(group, "geometry", models);
    ospCommit(group);

    ospRelease(model);
    ospRelease(models);

    OSPInstance instance = ospNewInstance(group);
        ospSetAffine3fv(instance, "xfm", affine_xform);
    ospCommit(instance);
    ospRelease(group);

    scene_instances.push_back(instance);

    return true;
}

bool
add_scene_object(const UpdateObject& update)
{    
    printf("OBJECT '%s' (scene)\n", update.name().c_str());    

    const std::string& linked_data = update.data_link();
    SceneDataTypeMap::iterator it = scene_data_types.find(linked_data);

    if (it == scene_data_types.end())
    {
        printf("--> '%s' | WARNING: no linked data found!\n", linked_data.c_str());
        return false;
    }
    else if (it->second != SDT_PLUGIN)
    {
        printf("--> '%s' | WARNING: linked data is not a plugin instance!\n", linked_data.c_str());
        return false;
    }
    else
        printf("--> '%s' (scene XXX?)\n", linked_data.c_str());

    PluginInstance* plugin_instance = plugin_instances[linked_data];
    assert(plugin_instance->type == PT_SCENE);
    PluginState *state = plugin_instance->state;

    GroupInstances instances = state->group_instances;

    if (instances.size() == 0)
        printf("... WARNING: no instances to add!\n");
    else
        printf("... Adding %d instances to scene!\n", instances.size());

    glm::mat4   obj2world;
    float       affine_xform[12];

    object2world_from_protobuf(obj2world, update);

    for (GroupInstance& gi : instances)
    {
        OSPGroup group = gi.first;
        const glm::mat4 instance_xform = gi.second;

        affine3fv_from_mat4(affine_xform, obj2world * instance_xform);

        OSPInstance instance = ospNewInstance(group);
            ospSetAffine3fv(instance, "xfm", affine_xform);
        ospCommit(instance);
        //ospRelease(group);

        scene_instances.push_back(instance);
    }

    // Lights
    const Lights& lights = state->lights;
    if (lights.size() > 0)
    {
        printf("... Adding %d lights to scene!\n", lights.size());
        for (OSPLight light : state->lights)
        {
            // XXX Sigh, need to apply object2world transform manually
            scene_lights.push_back(light);
        }
    }

    return true;
}

bool
add_volume_object(const UpdateObject& update, const Volume& volume_settings)
{
    printf("OBJECT '%s' (volume)\n", update.name().c_str());   

    const std::string& linked_data = update.data_link(); 
    SceneDataTypeMap::iterator it = scene_data_types.find(linked_data);

    if (it == scene_data_types.end())
    {
        printf("--> '%s' | WARNING: no linked data found!\n", linked_data.c_str());
        return false;
    }
    else if (it->second != SDT_PLUGIN)
    {
        printf("--> '%s' | WARNING: linked data is not a plugin instance!\n", linked_data.c_str());
        return false;
    }    
    else
        printf("--> '%s' (OSPRay volume?)\n", linked_data.c_str());

    PluginInstance* plugin_instance = plugin_instances[linked_data];
    assert(plugin_instance->type == PT_VOLUME);
    PluginState *state = plugin_instance->state;

    OSPVolume volume = state->volume;
    assert(volume != NULL);    

    const char *s_custom_properties = update.custom_properties().c_str();
    //printf("Received custom properties:\n%s\n", s_custom_properties);
    const json &custom_properties = json::parse(s_custom_properties);
    printf("Custom properties:\n");
    printf("%s\n", custom_properties.dump(4).c_str());

    OSPVolumetricModel volume_model = ospNewVolumetricModel(volume);

        // Set up further volume properties
        // XXX not sure these are handled correctly, and working in API2

        ospSetFloat(volume_model,  "samplingRate", volume_settings.sampling_rate());

        /*
        if (properties.find("gradient_shading") != properties.end())
            ospSetBool(volume_model,  "gradientShadingEnabled", properties["gradient_shading"].get<bool>());
        else
            ospSetBool(volume_model,  "gradientShadingEnabled", false);

        if (properties.find("pre_integration") != properties.end())
            ospSetBool(volume_model,  "preIntegration", properties["pre_integration"].get<bool>());
        else
            ospSetBool(volume_model,  "preIntegration", false);

        if (properties.find("single_shade") != properties.end())
            ospSetBool(volume_model,  "singleShade", properties["single_shade"].get<bool>());
        else
            ospSetBool(volume_model,  "singleShade", true);

        ospSetBool(volume_model, "adaptiveSampling", false);
        */

        // XXX need plugin instance for the volume_data_range
        OSPTransferFunction tf = create_transfer_function("cool2warm", 0.0f, 5.1f);
        ospSetObject(volume_model, "transferFunction", tf);
        ospRelease(tf);

    ospCommit(volume_model);

    if (current_renderer_type == "pathtracer")
    {

        OSPMaterial volumetricMaterial = ospNewMaterial(current_renderer_type.c_str(), "VolumetricMaterial");
            ospSetFloat(volumetricMaterial, "meanCosine", 0.f);
            ospSetVec3f(volumetricMaterial, "albedo", 1.f, 1.f, 1.f);
        ospCommit(volumetricMaterial);

        ospSetObject(volume_model, "material", volumetricMaterial);
        ospRelease(volumetricMaterial);

        ospCommit(volume_model);
    }

    OSPGroup group = ospNewGroup();
        OSPData data = ospNewData(1, OSP_OBJECT, &volume_model, 0);
        ospSetObject(group, "volume", data);                        // XXX why ospSetObject?
        //ospRelease(volume_model);
    ospCommit(group);

    glm::mat4   obj2world;
    float       affine_xform[12];

    object2world_from_protobuf(obj2world, update);
    affine3fv_from_mat4(affine_xform, obj2world);

    OSPInstance instance = ospNewInstance(group);
        ospSetAffine3fv(instance, "xfm", affine_xform);
    ospCommit(instance);
    ospRelease(group);

    scene_instances.push_back(instance);

#if 0
    // See https://github.com/ospray/ospray/pull/165, support for volume transformations was reverted
    ospSetVec3f(volume, "xfm.l.vx", osp::vec3f{ obj2world[0], obj2world[4], obj2world[8] });
    ospSetVec3f(volume, "xfm.l.vy", osp::vec3f{ obj2world[1], obj2world[5], obj2world[9] });
    ospSetVec3f(volume, "xfm.l.vz", osp::vec3f{ obj2world[2], obj2world[6], obj2world[10] });
    ospSetVec3f(volume, "xfm.p", osp::vec3f{ obj2world[3], obj2world[7], obj2world[11] });
#endif

    return true;
}

bool
add_isosurfaces_object(const UpdateObject& update)
{
    printf("OBJECT '%s' (isosurfaces)\n", update.name().c_str());    

    const std::string& linked_data = update.data_link();    
    SceneDataTypeMap::iterator it = scene_data_types.find(linked_data);

    if (it == scene_data_types.end())
    {
        printf("--> '%s' | WARNING: no linked data found!\n", linked_data.c_str());
        return false;
    }
    else if (it->second != SDT_PLUGIN)
    {
        printf("--> '%s' | WARNING: linked data is not a plugin instance!\n", linked_data.c_str());
        return false;
    }    
    else
        printf("--> '%s' (OSPRay volume?)\n", linked_data.c_str());

    PluginInstance* plugin_instance = plugin_instances[linked_data];
    assert(plugin_instance->type == PT_VOLUME);
    PluginState *state = plugin_instance->state;

    OSPVolume volume = state->volume;
    assert(volume != NULL);    

    const char *s_custom_properties = update.custom_properties().c_str();
    //printf("Received custom properties:\n%s\n", s_custom_properties);
    const json &custom_properties = json::parse(s_custom_properties);
    printf("... custom properties:\n");
    printf("%s\n", custom_properties.dump(4).c_str());
    
    if (custom_properties.find("isovalues") == custom_properties.end())
    {
        printf("WARNING: no property 'isovalues' set on object!\n");
        return false;
    }

    const json& isovalues_prop = custom_properties["isovalues"];
    int n = isovalues_prop.size();

    float *isovalues = new float[n];
    for (int i = 0; i < n; i++)
    {        
        isovalues[i] = isovalues_prop[i];
        printf("... isovalue #%d: %.3f\n", i, isovalues[i]);
    }

    OSPData isovalues_data = ospNewData(n, OSP_FLOAT, isovalues);
    ospCommit(isovalues_data);	// XXX double-check if a commit is needed for ospNewData, tut examples don't seem to use it
    delete [] isovalues;

    // XXX hacked temp volume module
    auto volumeModel = ospNewVolumetricModel(volume);
        OSPTransferFunction tf = create_transfer_function("cool2warm", 0.0f, 5.1f);
        ospSetObject(volumeModel, "transferFunction", tf);
        ospRelease(tf);
        //ospSetFloat(volumeModel, "samplingRate", 0.5f);
  	ospCommit(volumeModel);

    OSPGeometry isosurface = ospNewGeometry("isosurfaces");

        ospSetObject(isosurface, "volume", volumeModel);       		// XXX structured vol example indicates this needs to be the volume model??
        ospRelease(volume);

        ospSetData(isosurface, "isovalue", isovalues_data);
        ospRelease(isovalues_data);

    ospCommit(isosurface);

    OSPGeometricModel model = ospNewGeometricModel(isosurface);
        ospSetObject(model, "material", default_material);
    ospCommit(model);
    ospRelease(isosurface);
    
    OSPGroup group = ospNewGroup();
        OSPData data = ospNewData(1, OSP_OBJECT, &model, 0);
        ospSetObject(group, "geometry", data);                  // SetObject or SetData?
        //ospRelease(model);
    ospCommit(group);
 
    glm::mat4   obj2world;
    float       affine_xform[12];

    object2world_from_protobuf(obj2world, update);
    affine3fv_from_mat4(affine_xform, obj2world);

    OSPInstance instance = ospNewInstance(group);
        ospSetAffine3fv(instance, "xfm", affine_xform);
    ospCommit(instance);
    ospRelease(group);

    scene_instances.push_back(instance);
    
    return true;
}

bool
add_slices_object(const UpdateObject& update, const Slices& slices)
{
    printf("OBJECT '%s' (slices)\n", update.name().c_str());
    
    const std::string& linked_data = update.data_link(); 
    SceneDataTypeMap::iterator it = scene_data_types.find(linked_data);

    if (it == scene_data_types.end())
    {
        printf("--> '%s' | WARNING: no linked data found!\n", linked_data.c_str());
        return false;
    }
    else if (it->second != SDT_PLUGIN)
    {
        printf("--> '%s' | WARNING: linked data is not a plugin instance!\n", linked_data.c_str());
        return false;
    }    
    else
        printf("--> '%s' (OSPRay volume?)\n", linked_data.c_str());

    PluginInstance* plugin_instance = plugin_instances[linked_data];
    assert(plugin_instance->type == PT_VOLUME);
    PluginState *state = plugin_instance->state;

    OSPVolume volume = state->volume;
    assert(volume != NULL);    

    const char *s_custom_properties = update.custom_properties().c_str();
    //printf("Received custom properties:\n%s\n", s_custom_properties);
    const json &custom_properties = json::parse(s_custom_properties);
    printf("... custom properties:\n");
    printf("%s\n", custom_properties.dump(4).c_str());
    
    if (custom_properties.find("slice_plane") == custom_properties.end())
    {
        printf("WARNING: no property 'slice_plane' set on object!\n");
        return false;
    }
    
    // XXX Only a single slice plane at
    json slice_plane_prop = custom_properties["slice_plane"];
            
    if (slice_plane_prop.size() != 4)
    {
        fprintf(stderr, "ERROR: slice_plane attribute should contain list of 4 floats values!\n");
        return false;
    }

    float plane[4];

    for (int i = 0; i < slices.slices_size(); i++)
    {
        const Slice& slice = slices.slices(i);

        plane[0] = slice.a();
        plane[1] = slice.b();
        plane[2] = slice.c();
        plane[3] = slice.d();

        printf("... plane[%d]: %.3f, %3f, %.3f, %.3f\n", i, plane[0], plane[1], plane[2], plane[3]);

        OSPData planeData = ospNewData(1, OSP_VEC4F, plane);
        ospCommit(planeData);

            // XXX hacked temp volume module
        auto volumeModel = ospNewVolumetricModel(volume);
            OSPTransferFunction tf = create_transfer_function("cool2warm", 0.0f, 5.1f);
            ospSetObject(volumeModel, "transferFunction", tf);
            ospRelease(tf);
            //ospSetFloat(volumeModel, "samplingRate", 0.5f);
        ospCommit(volumeModel);

        OSPGeometry slice_geometry = ospNewGeometry("slices");
            ospSetObject(slice_geometry, "volume", volumeModel);         // XXX should be volume model
            //ospRelease(volume);
            ospSetData(slice_geometry, "plane", planeData);
            ospRelease(planeData);
        ospCommit(slice_geometry);
            
        OSPGeometricModel model = ospNewGeometricModel(slice_geometry);
            ospSetObject(model, "material", default_material);
        ospCommit(model);
        ospRelease(slice_geometry);
        
        OSPGroup group = ospNewGroup();
            OSPData data = ospNewData(1, OSP_OBJECT, &model, 0);
            ospSetObject(group, "geometry", data);                  // SetObject or SetData?
            //ospRelease(model);
        ospCommit(group);
     
        glm::mat4   obj2world;
        float       affine_xform[12];

        object2world_from_protobuf(obj2world, update);
        affine3fv_from_mat4(affine_xform, obj2world);

        OSPInstance instance = ospNewInstance(group);
            ospSetAffine3fv(instance, "xfm", affine_xform);
        ospCommit(instance);
        ospRelease(group);

        scene_instances.push_back(instance);
    }
    
    return true;
}

bool
add_light_object(const UpdateObject& update, const Light& light)
{
    OSPLight osp_light;

    printf("OBJECT '%s' (light)\n", light.object_name().c_str());
    printf("--> '%s' (blender light data)\n", light.light_name().c_str());    // XXX not set for ambient

    // XXX turn into render setting
    if (light.type() == Light::AMBIENT)
    {        
        osp_light = ospNewLight("ambient");
        ospSetFloat(osp_light, "intensity", light.intensity());
        ospSetVec3f(osp_light, "color", light.color(0), light.color(1), light.color(2));
        ospCommit(osp_light);
        scene_lights.push_back(osp_light);
        return true;
    }

    if (light.type() == Light::POINT)
    {
        osp_light = ospNewLight("sphere");
    }
    else if (light.type() == Light::SPOT)
    {
        osp_light = ospNewLight("spot");
        ospSetFloat(osp_light, "openingAngle", light.opening_angle());
        ospSetFloat(osp_light, "penumbraAngle", light.penumbra_angle());
    }
    else if (light.type() == Light::SUN)
    {
        osp_light = ospNewLight("distant");
        ospSetFloat(osp_light, "angularDiameter", light.angular_diameter());
    }
    else if (light.type() == Light::AREA)
    {
        // XXX blender's area light is more general than ospray's quad light
        osp_light = ospNewLight("quad");
        ospSetVec3f(osp_light, "edge1", light.edge1(0), light.edge1(1), light.edge1(2));
        ospSetVec3f(osp_light, "edge2", light.edge2(0), light.edge2(1), light.edge2(2));
    }
    //else
    // XXX HDRI

    printf("... intensity %.3f, visible %d\n", light.intensity(), light.visible());

    ospSetVec3f(osp_light, "color", light.color(0), light.color(1), light.color(2));
    ospSetFloat(osp_light, "intensity", light.intensity());
    ospSetBool(osp_light, "visible", light.visible());

    if (light.type() != Light::SUN)
        ospSetVec3f(osp_light, "position", light.position(0), light.position(1), light.position(2));

    if (light.type() == Light::SUN || light.type() == Light::SPOT)
        ospSetVec3f(osp_light, "direction", light.direction(0), light.direction(1), light.direction(2));

    if (light.type() == Light::POINT || light.type() == Light::SPOT)
        ospSetFloat(osp_light, "radius", light.radius());

    ospCommit(osp_light);

    scene_lights.push_back(osp_light);

    return true;
}

bool
handle_update_object(TCPSocket *sock)
{
    UpdateObject    update;    
    Light           light;

    if (!receive_protobuf(sock, update))
        return false;

    //print_protobuf(update);

    /*
    Not used on objects atm
    const char *s_custom_properties = update.custom_properties().c_str();
    printf("Received custom properties:\n%s\n", s_custom_properties);
    const json &custom_properties = json::parse(s_custom_properties);
    printf("Custom properties:\n");
    printf("%s\n", custom_properties.dump(4).c_str());
    */

    switch (update.type())
    {
    case UpdateObject::MESH:
        add_blender_mesh(update);
        break;

    case UpdateObject::GEOMETRY:
        add_geometry_object(update);
        break;

    case UpdateObject::SCENE:
        add_scene_object(update);
        break;

    case UpdateObject::VOLUME:
        {
        Volume volume;
        if (!receive_protobuf(sock, volume))
            return false;
        add_volume_object(update, volume);
        }
        break;

    case UpdateObject::ISOSURFACES:
        add_isosurfaces_object(update);
        break;
    
    case UpdateObject::SLICES:
        {
        Slices slices;
        if (!receive_protobuf(sock, slices))
            return false;
        add_slices_object(update, slices);
        }
        break;

    case UpdateObject::LIGHT:
        if (!receive_protobuf(sock, light))
            return false;
        add_light_object(update, light);
        break;

    default:
        printf("WARNING: unhandled update type %s\n", UpdateObject_Type_descriptor()->FindValueByNumber(update.type())->name().c_str());
        break;
    }

    return true;
}


// XXX currently has big memory leak as we never release the new objects ;-)
bool
receive_scene(TCPSocket *sock)
{
    // Image settings

    receive_protobuf(sock, image_settings);

    if (framebuffer_width != image_settings.width() || framebuffer_height != image_settings.height())
    {
        framebuffer_width = image_settings.width() ;
        framebuffer_height = image_settings.height();

        if (framebuffer_created)
            ospRelease(framebuffer);

        printf("Initializing framebuffer of %dx%d pixels\n", framebuffer_width, framebuffer_height);

        framebuffer = ospNewFrameBuffer(framebuffer_width, framebuffer_height, OSP_FB_RGBA32F, OSP_FB_COLOR | /*OSP_FB_DEPTH |*/ OSP_FB_ACCUM);
        // XXX is this call needed here?
        ospResetAccumulation(framebuffer);

        framebuffer_created = true;
    }

    // Render settings

    receive_protobuf(sock, render_settings);

    const std::string& renderer_type = render_settings.renderer();

    // Reuse existing renderer
    current_renderer_type = renderer_type;
    renderer = renderers[current_renderer_type.c_str()];

    printf("Background color %f, %f, %f, %f\n", render_settings.background_color(0),
        render_settings.background_color(1),
        render_settings.background_color(2),
        render_settings.background_color(3));    

    if (renderer_type == "scivis")
    {
        ospSetInt(renderer, "aoSamples", render_settings.ao_samples());

        ospSetVec4f(renderer, "bgColor",
            render_settings.background_color(0),
            render_settings.background_color(1),
            render_settings.background_color(2),
            render_settings.background_color(3));
    }
    else
    {
        // Pathtracer, work around unsupported bgColor

        // XXX appears to be broken? The backplate color *is* used, but only in areas where
        // there is non-zero alpha, i.e. something is hit.
        float texel[4] = { 
            /*render_settings.background_color(0),
            render_settings.background_color(1),
            render_settings.background_color(2),
            render_settings.background_color(3)*/
            0.0f, 1.0f, 0.0f, 1.0f
        };

        OSPData data = ospNewData(1, OSP_VEC4F, texel);

        OSPTexture backplate = ospNewTexture("texture2D");    
            ospSetVec2i(backplate, "size", 1, 1);
            ospSetInt(backplate, "type", OSP_TEXTURE_RGBA32F);
            ospSetData(backplate, "data", data);
        ospCommit(backplate);            
        ospRelease(data);

        //ospSetObject(renderer, "backplate", backplate);
        ospCommit(renderer);    
        ospRelease(backplate);
    }

    //ospSetBool(renderer, "shadowsEnabled", render_settings.shadows_enabled());        // XXX removed in 2.0?
    //ospSetInt(renderer, "spp", 1);

    ospCommit(renderer);

    default_material = materials[renderer_type.c_str()];

    // Update camera

    receive_protobuf(sock, camera_settings);

    printf("OBJECT '%s' (camera)\n", camera_settings.object_name().c_str());
    printf("--> '%s' (camera data)\n", camera_settings.camera_name().c_str());

    float cam_pos[3], cam_viewdir[3], cam_updir[3];

    cam_pos[0] = camera_settings.position(0);
    cam_pos[1] = camera_settings.position(1);
    cam_pos[2] = camera_settings.position(2);

    cam_viewdir[0] = camera_settings.view_dir(0);
    cam_viewdir[1] = camera_settings.view_dir(1);
    cam_viewdir[2] = camera_settings.view_dir(2);

    cam_updir[0] = camera_settings.up_dir(0);
    cam_updir[1] = camera_settings.up_dir(1);
    cam_updir[2] = camera_settings.up_dir(2);

    switch (camera_settings.type())
    {
        case CameraSettings::PERSPECTIVE:
            camera = ospNewCamera("perspective");
            ospSetFloat(camera, "fovy",  camera_settings.fov_y());
            break;

        case CameraSettings::ORTHOGRAPHIC:
            camera = ospNewCamera("orthographic");
            ospSetFloat(camera, "height", camera_settings.height());
            break;

        case CameraSettings::PANORAMIC:
            camera = ospNewCamera("panoramic");
            break;

        default:
            fprintf(stderr, "WARNING: unknown camera type %d\n", camera_settings.type());
            break;
    }

    ospSetFloat(camera, "aspect", camera_settings.aspect());        // XXX panoramic only
    ospSetFloat(camera, "nearClip", camera_settings.clip_start());

    ospSetVec3fv(camera, "position", cam_pos);
    ospSetVec3fv(camera, "direction", cam_viewdir);
    ospSetVec3fv(camera, "up",  cam_updir);

    if (camera_settings.dof_focus_distance() > 0.0f)
    {
        // XXX seem to stuck in loop during rendering when distance is 0
        ospSetFloat(camera, "focusDistance", camera_settings.dof_focus_distance());
        ospSetFloat(camera, "apertureRadius", camera_settings.dof_aperture());
    }

    if (image_settings.border_size() == 4)
    {
        ospSetVec2f(camera, "imageStart", image_settings.border(0), image_settings.border(1));
        ospSetVec2f(camera, "imageEnd", image_settings.border(2), image_settings.border(3));
    }

    ospCommit(camera);

    // Done!

    return true;
}

// Send result

size_t
write_framebuffer_exr(const char *fname)
{
    // Access framebuffer
    const float *fb = (float*)ospMapFrameBuffer(framebuffer, OSP_FB_COLOR);

    writeEXRFramebuffer(fname, framebuffer_width, framebuffer_height, fb);

    // Unmap framebuffer
    ospUnmapFrameBuffer(fb, framebuffer);

    struct stat st;
    stat(fname, &st);

    return st.st_size;
}

/*
// Not used atm, as we use sendfile()
void
send_framebuffer(TCPSocket *sock)
{
    uint32_t bufsize;

    struct timeval t0, t1;
    gettimeofday(&t0, NULL);

    // Access framebuffer
    const float *fb = (float*)ospMapFrameBuffer(framebuffer, OSP_FB_COLOR);

#if 1
    // Write to OpenEXR file and send *its* contents
    const char *FBFILE = "/dev/shm/orsfb.exr";

    // XXX this also maps/unmaps the framebuffer!
    size_t size = write_framebuffer_exr(FBFILE);

    printf("Sending framebuffer as OpenEXR file, %d bytes\n", size);

    bufsize = size;
    sock->send((uint8_t*)&bufsize, 4);
    sock->sendfile(FBFILE);
#else
    // Send directly
    bufsize = framebuffer_width*framebuffer_height*4*4;

    printf("Sending %d bytes of framebuffer data\n", bufsize);

    sock->send(&bufsize, 4);
    sock->sendall((uint8_t*)fb, framebuffer_width*framebuffer_height*4*4);
#endif

    // XXX can already unmap after written to file
    // Unmap framebuffer
    ospUnmapFrameBuffer(fb, framebuffer);

    gettimeofday(&t1, NULL);
    printf("Sent framebuffer in %.3f seconds\n", time_diff(t0, t1));
}
*/

// Rendering

void
render_thread_func(BlockingQueue<ClientMessage>& render_input_queue,
    BlockingQueue<RenderResult>& render_result_queue)
{
    struct timeval t0, t1, t2;
    size_t  framebuffer_file_size;
    char fname[1024];

    gettimeofday(&t0, NULL);

    // Clear framebuffer
    // XXX no 2.0 equivalent?
    //ospFrameBufferClear(framebuffer, OSP_FB_COLOR | OSP_FB_ACCUM);
    ospResetAccumulation(framebuffer);

    for (int i = 1; i <= render_settings.samples(); i++)
    {
        printf("Rendering sample %d ... ", i);
        fflush(stdout);

        gettimeofday(&t1, NULL);

        /*
        What to render and how to render it depends on the renderer's parameters. If
        the framebuffer supports accumulation (i.e., it was created with
        `OSP_FB_ACCUM`) then successive calls to `ospRenderFrame` will progressively
        refine the rendered image. If additionally the framebuffer has an
        `OSP_FB_VARIANCE` channel then `ospRenderFrame` returns an estimate of the
        current variance of the rendered image, otherwise `inf` is returned. The
        estimated variance can be used by the application as a quality indicator and
        thus to decide whether to stop or to continue progressive rendering.
        */
        ospRenderFrame(framebuffer, renderer, camera, world);

        gettimeofday(&t2, NULL);
        printf("frame in %.3f seconds\n", time_diff(t1, t2));

        // Save framebuffer to file
        sprintf(fname, "/dev/shm/blosprayfb%04d.exr", i);

        framebuffer_file_size = write_framebuffer_exr(fname);
        // XXX check result value

        // Signal a new frame is available
        RenderResult rs;
        rs.set_type(RenderResult::FRAME);
        rs.set_sample(i);
        rs.set_file_name(fname);
        rs.set_file_size(framebuffer_file_size);
        rs.set_memory_usage(memory_usage());

        render_result_queue.push(rs);

        // XXX handle cancel input

        if (render_input_queue.size() > 0)
        {
            ClientMessage cm = render_input_queue.pop();

            if (cm.type() == ClientMessage::CANCEL_RENDERING)
            {
                printf("{render thread} Canceling rendering\n");

                RenderResult rs;
                rs.set_type(RenderResult::CANCELED);
                render_result_queue.push(rs);

                return;
            }
        }
    }

    RenderResult rs;
    rs.set_type(RenderResult::DONE);
    render_result_queue.push(rs);

    gettimeofday(&t2, NULL);
    printf("Rendering done in %.3f seconds\n", time_diff(t0, t2));
}

// Querying

bool
handle_query_bound(TCPSocket *sock, const std::string& name)
{
    QueryBoundResult result;

    PluginStateMap::const_iterator it = plugin_state.find(name);

    if (it == plugin_state.end())
    {
        char msg[1024];
        sprintf(msg, "No plugin state for id '%s'", name.c_str());

        result.set_success(false);
        result.set_message(msg);

        send_protobuf(sock, result);

        return false;
    }

    const PluginState *state = it->second;

    BoundingMesh *bound = state->bound;

    if (bound)
    {
        uint32_t    size;
        uint8_t     *buffer = bound->serialize(size);

        result.set_success(true);
        result.set_result_size(size);

        send_protobuf(sock, result);
        sock->sendall(buffer, size);
    }
    else
    {
        result.set_success(false);
        result.set_message("No bound specified");
        send_protobuf(sock, result);
    }

    return true;
}

bool
prepare_scene()
{
    printf("Setting up world with %d instance(s)\n", scene_instances.size());
    OSPData instances = ospNewData(scene_instances.size(), OSP_OBJECT, &scene_instances[0], 0);

    // XXX might not have to recreate world, only update instances
    world = ospNewWorld();
        // Check https://github.com/ospray/ospray/issues/277. Is bool setting fixed in 2.0?
        //ospSetBool(world, "compactMode", true);
        ospSetData(world, "instance", instances);
    ospCommit(world);
    ospRelease(instances);
    
    printf("Have %d light(s) in the scene\n", scene_lights.size());
    OSPData light_data = ospNewData(scene_lights.size(), OSP_OBJECT, &scene_lights[0], 0);
    ospSetData(renderer, "light", light_data);
    ospCommit(renderer);

    scene_instances.clear();        // XXX hmm, clearing scene here
    scene_lights.clear();

    return true;
}

// Connection handling

bool
handle_connection(TCPSocket *sock)
{
    BlockingQueue<ClientMessage> render_input_queue;
    BlockingQueue<RenderResult> render_result_queue;

    ClientMessage       client_message;
    RenderResult        render_result;
    RenderResult::Type  rr_type;

    std::thread         render_thread;
    bool                rendering = false;

    while (true)
    {
        // Check for new client message

        if (sock->is_readable())
        {
            if (!receive_protobuf(sock, client_message))
            {
                // XXX if we were rendering, handle the chaos

                fprintf(stderr, "Failed to receive client message (%d), goodbye!\n", sock->get_errno());
                sock->close();
                return false;
            }

            //printf("Got client message of type %s\n", ClientMessage_Type_Name(client_message.type()).c_str());
            //printf("%s\n", client_message.DebugString().c_str());

            switch (client_message.type())
            {
                case ClientMessage::UPDATE_SCENE:
                    // XXX handle clear_scene
                    // XXX check res
                    // XXX ignore if rendering
                    receive_scene(sock);
                    break;

                case ClientMessage::UPDATE_PLUGIN_INSTANCE:
                    handle_update_plugin_instance(sock);
                    break;

                case ClientMessage::UPDATE_BLENDER_MESH:
                    handle_update_blender_mesh(sock, client_message.string_value());
                    break;

                case ClientMessage::UPDATE_OBJECT:
                    handle_update_object(sock);
                    break;

                case ClientMessage::QUERY_BOUND:
                    handle_query_bound(sock, client_message.string_value());
                    //printf("WARNING: message ignored atm!\n");

                    return true;

                case ClientMessage::START_RENDERING:

                    if (rendering)
                    {
                        // Ignore
                        break;
                    }

                    //render_input_queue.clear();
                    //render_result_queue.clear();        // XXX handle any remaining results

                    // Setup world and scene objects
                    prepare_scene();

                    // Start render thread
                    render_thread = std::thread(&render_thread_func, std::ref(render_input_queue), std::ref(render_result_queue));

                    rendering = true;

                    break;

                case ClientMessage::CANCEL_RENDERING:

                    printf("Got request to CANCEL rendering\n");

                    if (!rendering)
                        break;

                    render_input_queue.push(client_message);

                    break;

                case ClientMessage::QUIT:
                    // XXX if we were still rendering, handle the chaos

                    printf("Got QUIT message\n");

                    sock->close();

                    return true;

                default:

                    printf("WARNING: unhandled client message!\n");
            }
        }

        // Check for new render results

        if (rendering && render_result_queue.size() > 0)
        {
            render_result = render_result_queue.pop();

            // Forward render results on socket
            send_protobuf(sock, render_result);

            switch (render_result.type())
            {
                case RenderResult::FRAME:
                    // New framebuffer (for a single sample) available, send
                    // it to the client

                    printf("Frame available, sample %d (%s, %d bytes)\n", render_result.sample(), render_result.file_name().c_str(), render_result.file_size());

                    sock->sendfile(render_result.file_name().c_str());

                    // Remove local framebuffer file
                    if (!keep_framebuffer_files)
                        unlink(render_result.file_name().c_str());

                    break;

                case RenderResult::CANCELED:
                    printf("Rendering canceled!\n");

                    // Thread should have finished by now
                    render_thread.join();

                    rendering = false;
                    break;

                case RenderResult::DONE:
                    printf("Rendering done!\n");

                    // Thread should have finished by now
                    render_thread.join();

                    rendering = false;
                    break;
            }
        }

        usleep(1000);
    }

    sock->close();

    return true;
}

// Error/status display

void
ospray_error(OSPError e, const char *error)
{
    printf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    printf("OSPRAY ERROR: %s\n", error);
    printf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
}

void
ospray_status(const char *message)
{
    printf("--------------------------------------------------\n");
    printf("OSPRAY STATUS: %s\n", message);
    printf("--------------------------------------------------\n");
}

// Main

int
main(int argc, const char **argv)
{
    // Verify that the version of the library that we linked against is
    // compatible with the version of the headers we compiled against.
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    // Initialize OSPRay.
    // OSPRay parses (and removes) its commandline parameters, e.g. "--osp:debug"
    ospInit(&argc, argv);

    ospDeviceSetErrorFunc(ospGetCurrentDevice(), ospray_error);
    ospDeviceSetStatusFunc(ospGetCurrentDevice(), ospray_status);

    // Prepare some things
    prepare_renderers();

    // Server loop

    TCPSocket *listen_sock;

    listen_sock = new TCPSocket;
    listen_sock->bind(PORT);
    listen_sock->listen(1);

    printf("Listening on port %d\n", PORT);

    TCPSocket *sock;

    while (true)
    {
        printf("Waiting for new connection...\n");

        sock = listen_sock->accept();

        printf("Got new connection\n");

        if (!handle_connection(sock))
            printf("Error handling connection!\n");
        else
            printf("Connection successfully handled\n");
    }

    return 0;
}
