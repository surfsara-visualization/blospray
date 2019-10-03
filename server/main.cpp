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
#include "scene.h"

using json = nlohmann::json;

const int       PORT = 5909;
const uint32_t  PROTOCOL_VERSION = 2;

OSPRenderer     renderer;
std::string     current_renderer_type;
OSPWorld        world;
OSPCamera       camera = nullptr;
OSPFrameBuffer  framebuffer;

struct SceneMaterial
{
    MaterialUpdate::Type    type;
    OSPMaterial             material;

    SceneMaterial()
    {
        material = nullptr;
    }

    ~SceneMaterial()
    {
        ospRelease(material);
    }
};

typedef std::map<std::string, SceneMaterial*>  SceneMaterialMap;

std::map<std::string, OSPRenderer>  renderers;

std::map<std::string, OSPMaterial>  default_materials;
SceneMaterialMap            scene_materials;
std::string                 scene_materials_renderer;

std::vector<OSPInstance>    scene_instances;

OSPLight                    ambient_light;
std::vector<OSPLight>       scene_lights;

OSPData                     scene_instances_data = nullptr;
OSPData                     scene_lights_data = nullptr;

int                         framebuffer_width=0, framebuffer_height=0;
OSPFrameBufferFormat        framebuffer_format;
bool                        framebuffer_created = false;

int             render_samples=1;

bool            keep_framebuffer_files = getenv("BLOSPRAY_KEEP_FRAMEBUFFER_FILES") != nullptr;
bool            dump_client_messages = getenv("BLOSPRAY_DUMP_CLIENT_MESSAGES") != nullptr;
bool            abort_on_ospray_error = getenv("BLOSPRAY_ABORT_ON_OSPRAY_ERROR") != nullptr;

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
    std::string     name;

    PluginType      type;
    std::string     plugin_name;

    std::string     parameters_hash;
    std::string     custom_properties_hash;

    // XXX store hash of parameters that the instance was generated from

    // Plugin state contains OSPRay scene elements
    // XXX move properties out of PluginState?
    PluginState     *state;     // XXX store as object, not as pointer?
};

// A regular Blender Mesh XXX currently triangles only
struct BlenderMesh
{
    std::string     name;
    uint32_t        num_vertices;
    uint32_t        num_triangles;

    json            parameters;     // XXX not sure we need this

    OSPGeometry     geometry;
};

// Top-level scene objects
typedef std::map<std::string, SceneObject*>     SceneObjectMap;
// Type of each Mesh Data, either plugin or regular Blender meshe
typedef std::map<std::string, SceneDataType>    SceneDataTypeMap;

typedef std::map<std::string, PluginInstance*>  PluginInstanceMap;
typedef std::map<std::string, BlenderMesh*>     BlenderMeshMap;

SceneObjectMap      scene_objects;
SceneDataTypeMap    scene_data_types;
PluginInstanceMap   plugin_instances;
BlenderMeshMap      blender_meshes;

// Plugin handling

// If needed, loads plugin shared library and initializes plugin
// XXX perhaps this operation should have its own ...Result type
bool
ensure_plugin_is_loaded(GenerateFunctionResult &result, PluginDefinition &definition,
    PluginType type, const std::string& name)
{
    if (name == "")
    {
        printf("No plugin name provided!\n");
        return false;
    }

    std::string internal_name;

    switch (type)
    {
    case PT_VOLUME:
        internal_name = "volume";
        break;
    case PT_GEOMETRY:
        internal_name = "geometry";
        break;
    case PT_SCENE:
        internal_name = "scene";
        break;
    }

    internal_name += "_" + name;

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

            fprintf(stderr, "Failed to open plugin:\ndlopen() error: %s\n", dlerror());
            return false;
        }

        dlerror();  // Clear previous error

        // Initialize plugin

        plugin_initialization_function *initialize = (plugin_initialization_function*) dlsym(plugin, "initialize");

        if (initialize == NULL)
        {
            result.set_success(false);
            result.set_message("Failed to get initialization function from plugin!");

            fprintf(stderr, "Failed to get initialization function from plugin:\ndlsym() error: %s\n", dlerror());

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
delete_plugin_instance(const std::string& name)
{        
    PluginInstanceMap::iterator it = plugin_instances.find(name);

    if (it == plugin_instances.end())
    {
        printf("ERROR: plugin instance '%s' to delete not found!\n", name.c_str());
        return;
    }

    PluginInstance *plugin_instance = it->second;
    PluginState *state = plugin_instance->state;
    
    // Released OSPRay resources created by the plugin
    switch (plugin_instance->type)
    {
    case PT_GEOMETRY:
        if (state->geometry)
            ospRelease(state->geometry);
        break;
    case PT_VOLUME:
        if (state->volume)
            ospRelease(state->volume);
        break;
    case PT_SCENE:
        for (auto& kv: state->group_instances)
            ospRelease(kv.first);    
        for (OSPLight& l: state->lights)
            ospRelease(l);
        break;
    }

    if (state->bound)
        delete state->bound;
    //if (state->data)
    // XXX call plugin's clear_data_function_t

    delete state;    

    plugin_instances.erase(it);
    plugin_state.erase(name);
    scene_data_types.erase(name);
}

//
// Scene management
//

void
delete_object(const std::string& object_name)
{        
    SceneObjectMap::iterator it = scene_objects.find(object_name);

    if (it == scene_objects.end())
    {
        printf("ERROR: object to delete '%s' not found!\n", object_name.c_str());
        return;
    }

    SceneObject *scene_object = it->second;
    delete scene_object;

    scene_objects.erase(object_name);
}

void 
delete_scene_data(const std::string& name)
{
    SceneDataTypeMap::iterator it = scene_data_types.find(name);

    if (it == scene_data_types.end())
    {
        printf("ERROR: scene data '%s' to delete not found!\n", name.c_str());
        return;
    }

    if (it->second == SDT_PLUGIN)
        delete_plugin_instance(name);
    else
    {
        assert(it->second == SDT_MESH);
        // XXX todo
        //delete_blender_mesh(name);
    }

    scene_data_types.erase(name);
}

/*
Find scene object by name, create new if not found.
Three cases:
1. no existing object with name 
2. existing object with name, but of wrong type 
3. existing object with name and correct type 

Returns NULL if no existing object found with given name.
*/

SceneObject*
find_scene_object(const std::string& name, SceneObjectType type, bool delete_existing_mismatch=true)
{
    SceneObject *scene_object;
    SceneObjectMap::iterator it = scene_objects.find(name);

    if (it != scene_objects.end())
    {
        scene_object = it->second;
        if (scene_object->type != type)
        {
            if (delete_existing_mismatch)
            {
                printf("... Existing object is not of type %s, but of type %s, deleting\n", 
                    SceneObjectType_names[type], SceneObjectType_names[scene_object->type]);
                delete_object(name);
                return nullptr;
            }
            else
                return scene_object;
        }
        else
        {        
            printf("... Existing object matches type %s\n", SceneObjectType_names[type]);
            return scene_object;
        }
    }

    printf("... No existing object\n");

    return nullptr;
}

bool
scene_data_with_type_exists(const std::string& name, SceneDataType type)
{
    SceneDataTypeMap::iterator it = scene_data_types.find(name);

    if (it == scene_data_types.end())
    {
        printf("... Scene data '%s' does not exist\n", name.c_str());
        return false;
    }
    else if (it->second != type)
    {
        printf("... Scene data '%s' is not of type %s, but of type %s\n", 
            name.c_str(), SceneDataType_names[type], SceneDataType_names[it->second]);
        return false;
    }

    printf("... Scene data '%s' found, type %s\n", name.c_str(), SceneDataType_names[type]);
    
    return true;        
}

//
// Scene elements
//

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

        	OSPData color_data = ospNewData(cool2warm_entries, OSP_VEC3F, tf_colors, 0);
        	ospSetObject(tf, "color", color_data);        	

        	// XXX color and opacity can be decoupled?
        	OSPData opacity_data = ospNewData(cool2warm_entries, OSP_FLOAT, tf_opacities, 0);
        	ospSetObject(tf, "opacity", opacity_data);        	

    	ospCommit(tf);
        ospRelease(color_data);
        ospRelease(opacity_data);        

    	return tf;
	}

    return nullptr;
}

bool
handle_update_plugin_instance(TCPSocket *sock)
{
    UpdatePluginInstance    update;

    if (!receive_protobuf(sock, update))
        return false;

    //print_protobuf(update);

    const std::string& data_name = update.name();

    printf("PLUGIN INSTANCE '%s'\n", data_name.c_str());

    bool create_new_instance;
    PluginInstance *plugin_instance;
    PluginState *state;    
    PluginType plugin_type;

    switch (update.type())
    {
    case UpdatePluginInstance::GEOMETRY:
        plugin_type = PT_GEOMETRY;
        break;
    case UpdatePluginInstance::VOLUME:
        plugin_type = PT_VOLUME;
        break;
    case UpdatePluginInstance::SCENE:
        plugin_type = PT_SCENE;
        break;
    default:
        printf("... WARNING: unknown plugin instance type %d!\n", update.type());
        return false;
    }

    const char *plugin_type_name = PluginType_names[plugin_type];
    const std::string &plugin_name = update.plugin_name();

    printf("... plugin type: %s\n", plugin_type_name);
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

    // Check against the current instances

    create_new_instance = true;

    if (scene_data_with_type_exists(data_name, SDT_PLUGIN))
    {
        // Have existing plugin instance with this name, check what it is
        plugin_instance = plugin_instances[data_name];
        assert(plugin_state.find(data_name) != plugin_state.end());
        state = plugin_state[data_name];

        if (plugin_instance->type != plugin_type || plugin_instance->plugin_name != plugin_name)
        {
            printf("... Existing plugin (type %s, name '%s') does't match, overwriting!\n", 
                PluginType_names[plugin_instance->type], plugin_name.c_str());
            delete_plugin_instance(data_name);            
        }
        else
        {
            // Plugin still of the same type and name, check if parameters and properties still up to date
            const std::string& parameters_hash = get_sha1(update.plugin_parameters());
            const std::string& custom_props_hash = get_sha1(update.custom_properties());

            if (parameters_hash != plugin_instance->parameters_hash)
            {
                printf("... Parameters changed, re-running plugin\n");
                delete_plugin_instance(data_name);                
            }
            else if (custom_props_hash != plugin_instance->custom_properties_hash)
            {
                printf("... Custom properties changed, re-running plugin\n");
                delete_plugin_instance(data_name);                
            }
            else if (plugin_instance->state->uses_renderer_type && plugin_instance->state->renderer != current_renderer_type)
            {
                printf("... Plugin depends on renderer type, which changed from '%s', re-running plugin\n", 
                    plugin_instance->state->renderer.c_str());
                delete_plugin_instance(data_name);                
            }
            else
                create_new_instance = false;
        }
    }

    // Prepare result
    // By default all is well, we let the plugin signal something went wrong 
    GenerateFunctionResult result;    
    result.set_success(true);    

    if (!create_new_instance)
    {
        printf("... Cached plugin instance still up-to-date\n");
        // XXX we misuse GenerateFunctionResult here, as nothing was generated...
        send_protobuf(sock, result);
        return true;
    }

    // At this point we're creating a new plugin instance, check the plugin itself first

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
        printf("... ERROR: Plugin generate_function is NULL!\n");
        result.set_message("Plugin generate_function is NULL!");
        send_protobuf(sock, result);
        return false;
    }

    // Check parameters passed to generate function

    if (!check_plugin_parameters(result, plugin_definition.parameters, plugin_parameters))
    {
        // Something went wrong...
        send_protobuf(sock, result);
        return false;
    }    

    // Create plugin instance and state
    // XXX delete plugin instance and state below in case of error

    state = new PluginState; 
    state->renderer = current_renderer_type;   
    state->uses_renderer_type = plugin_definition.uses_renderer_type;
    state->parameters = plugin_parameters;

    // Call generate function

    struct timeval t0, t1;

    printf("... Calling generate function\n");
    gettimeofday(&t0, NULL);

    generate_function(result, state);

    gettimeofday(&t1, NULL);
    printf("... Generate function executed in %.3fs\n", time_diff(t0, t1));
    
    if (!result.success())
    {
        printf("... ERROR: generate function failed:\n");
        printf("... %s\n", result.message().c_str());
        send_protobuf(sock, result);
        delete state;
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

            printf("... ERROR: geometry generate function did not set an OSPGeometry!\n");
            delete state;
            return false;
        }    

        break;

    case UpdatePluginInstance::VOLUME:

        if (state->volume == NULL)
        {
            send_protobuf(sock, result);

            printf("... ERROR: volume generate function did not set an OSPVolume!\n");
            delete state;
            return false;
        }

        break;

    case UpdatePluginInstance::SCENE:

        if (state->group_instances.size() == 0)
            printf("... WARNING: scene generate function returned 0 instances!\n");    

        break;
    }

    // Load function succeeded

    plugin_instance = new PluginInstance;
    plugin_instance->type = plugin_type;
    plugin_instance->plugin_name = plugin_name;        
    plugin_instance->state = state; 
    plugin_instance->name = data_name;    
    plugin_instance->parameters_hash = get_sha1(s_plugin_parameters);
    plugin_instance->custom_properties_hash = get_sha1(s_custom_properties);    

    plugin_instances[data_name] = plugin_instance;
    plugin_state[data_name] = state;
    scene_data_types[data_name] = SDT_PLUGIN;

    send_protobuf(sock, result);

    return true;
}

bool
handle_update_blender_mesh_data(TCPSocket *sock, const std::string& name)
{
    printf("DATA '%s' (blender mesh)\n", name.c_str());

    BlenderMesh *blender_mesh;
    OSPGeometry geometry;
    bool create_new_mesh = false;

    SceneDataTypeMap::iterator it = scene_data_types.find(name);
    if (it == scene_data_types.end())
    {
        // No previous mesh with this name
        printf("... Unseen name, creating new mesh\n");
        create_new_mesh = true;
    }
    else
    {
        // Have existing scene data with this name, check what it is
        SceneDataType type = it->second;

        if (type != SDT_MESH)
        {
            printf("... WARNING: data is currently of type %s, overwriting with new mesh!\n", SceneDataType_names[type]);
            delete_scene_data(name);
            create_new_mesh = true;
        }
        else
        {
            printf("... Updating existing mesh\n");            
            blender_mesh = blender_meshes[name];
            geometry = blender_mesh->geometry;
            // As we're updating an existing geometry these might not get set 
            // again below, so remove them here. If they are set below there
            // value will get updated anyway
            // XXX is it ok to remove a param that was never set?
            ospRemoveParam(geometry, "vertex.normal");
            ospRemoveParam(geometry, "vertex.color");
        }
    }

    if (create_new_mesh)
    {
        blender_mesh = blender_meshes[name] = new BlenderMesh;
        geometry = blender_mesh->geometry = ospNewGeometry("triangles");
        scene_data_types[name] = SDT_MESH;
    }

    MeshData    mesh_data;
    OSPData     data;
    uint32_t    nv, nt, flags;    

    if (!receive_protobuf(sock, mesh_data))
        return false;

    nv = blender_mesh->num_vertices = mesh_data.num_vertices();
    nt = blender_mesh->num_triangles = mesh_data.num_triangles();
    flags = mesh_data.flags();

    printf("... %d vertices, %d triangles, flags 0x%08x\n", nv, nt, flags);

    // Receive mesh data

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

    // Set up geometry

    data = ospNewData(nv, OSP_VEC3F, &vertex_buffer[0], 0);    
    ospSetObject(geometry, "vertex.position", data);
    ospRelease(data);

    if (flags & MeshData::NORMALS)
    {
        data = ospNewData(nv, OSP_VEC3F, &normal_buffer[0], 0);        
        ospSetObject(geometry, "vertex.normal", data);
        ospRelease(data);
    }

    if (flags & MeshData::VERTEX_COLORS)
    {
        data = ospNewData(nv, OSP_VEC4F, &vertex_color_buffer[0], 0);        
        ospSetObject(geometry, "vertex.color", data);
        ospRelease(data);
    }

    data = ospNewData(nt, OSP_VEC3UI, &triangle_buffer[0], 0);    
    ospSetObject(geometry, "index", data);
    ospRelease(data);

    ospCommit(geometry);

    return true;
}

bool
update_blender_mesh_object(const UpdateObject& update)
{    
    const std::string& object_name = update.name();
    const std::string& linked_data = update.data_link();

    printf("OBJECT '%s' (blender mesh)\n", object_name.c_str());   
    printf("--> '%s'\n", linked_data.c_str());

    SceneObject     *scene_object;
    SceneObjectMesh *mesh_object;
    OSPInstance     instance;
    OSPGroup        group;
    OSPGeometricModel gmodel;

    scene_object = find_scene_object(object_name, SOT_MESH);

    if (scene_object == nullptr)
        mesh_object = new SceneObjectMesh;    
    else
        mesh_object = dynamic_cast<SceneObjectMesh*>(scene_object);

    instance = mesh_object->instance;
    assert(instance != nullptr);
    group = mesh_object->group;
    assert(group != nullptr);

    // Check linked data

    if (!scene_data_with_type_exists(linked_data, SDT_MESH))
    {
        if (scene_object == nullptr)
            delete mesh_object;
        return false;
    }

    BlenderMesh *blender_mesh = blender_meshes[linked_data];
    OSPGeometry geometry = blender_mesh->geometry;

    if (geometry == NULL)
    {
        printf("... ERROR: geometry is NULL!\n");
        if (scene_object == nullptr)
            delete mesh_object;
        return false;
    }    

    if (scene_object == nullptr)
    {
        mesh_object->data_link = linked_data;
        gmodel = mesh_object->gmodel = ospNewGeometricModel(geometry);
    }
    else
    {
        // XXX need this for updating material
        gmodel = mesh_object->gmodel;
    }

    // Update object 

    glm::mat4   obj2world;
    float       affine_xform[12];

    object2world_from_protobuf(obj2world, update);
    affine3fv_from_mat4(affine_xform, obj2world);
    ospSetParam(instance, "xfm", OSP_AFFINE3F, affine_xform);

    ospCommit(instance);    
    
    ospSetObjectAsData(group, "geometry", OSP_GEOMETRIC_MODEL, gmodel);
    ospCommit(group);    

    const std::string& matname = update.material_link();

    SceneMaterialMap::iterator it = scene_materials.find(matname);
    if (it != scene_materials.end())
    {
        printf("... Material '%s'\n", matname.c_str());
        ospSetObjectAsData(gmodel, "material", OSP_MATERIAL, it->second->material);
    }
    else
    {
        printf("... WARNING: Material '%s' not found, using default!\n", matname.c_str());
        ospSetObjectAsData(gmodel, "material", OSP_MATERIAL, default_materials[current_renderer_type]);
    }

    /*
    float cols[] = { 1, 0, 0, 1 };
    OSPData colors = ospNewData(1, OSP_VEC4F, &cols[0], 0);        
    ospSetObject(gmodel, "color", colors);
    ospRelease(colors);
    */

    ospCommit(gmodel);

    if (scene_object == nullptr)
        scene_objects[object_name] = mesh_object;

    // XXX should create this list from scene_objects?
    scene_instances.push_back(instance);

    return true;
}


bool
update_geometry_object(const UpdateObject& update)
{   
    const std::string& object_name = update.name();
    const std::string& linked_data = update.data_link(); 

    printf("OBJECT '%s' (geometry)\n", object_name.c_str());    
    printf("--> '%s'\n", linked_data.c_str());

    SceneObject     *scene_object;
    SceneObjectGeometry *geometry_object;
    OSPInstance     instance;
    OSPGroup        group;
    OSPGeometricModel gmodel;

    scene_object = find_scene_object(object_name, SOT_GEOMETRY);

    if (scene_object == nullptr)
        geometry_object = new SceneObjectGeometry;
    else
        geometry_object = dynamic_cast<SceneObjectGeometry*>(scene_object);

    instance = geometry_object->instance;
    assert(instance != nullptr);
    group = geometry_object->group;
    assert(group != nullptr);        

    // Check linked data    
    
    if (!scene_data_with_type_exists(linked_data, SDT_PLUGIN))
    {
        if (scene_object == nullptr)
            delete geometry_object;
        return false;
    }

    PluginInstance* plugin_instance = plugin_instances[linked_data];
    assert(plugin_instance->type == PT_GEOMETRY);
    PluginState *state = plugin_instance->state;

    OSPGeometry geometry = state->geometry;

    if (geometry == NULL)
    {
        printf("... ERROR: geometry is NULL!\n");
        if (scene_object == nullptr)
            delete geometry_object;
        return false;
    }    

    if (scene_object == nullptr)
    {
        gmodel = geometry_object->gmodel = ospNewGeometricModel(geometry); 

        ospSetObjectAsData(group, "geometry", OSP_GEOMETRIC_MODEL, gmodel);
        ospCommit(group);
    }
    else
        gmodel = geometry_object->gmodel;

    glm::mat4   obj2world;
    float       affine_xform[12];

    object2world_from_protobuf(obj2world, update);
    affine3fv_from_mat4(affine_xform, obj2world);

    ospSetParam(instance, "xfm", OSP_AFFINE3F, affine_xform);    
    ospCommit(instance);

    const std::string& matname = update.material_link();

    SceneMaterialMap::iterator it = scene_materials.find(matname);
    if (it != scene_materials.end())
    {
        printf("... Material '%s'\n", matname.c_str()); 
        ospSetObjectAsData(gmodel, "material", OSP_MATERIAL, it->second->material);
    }
    else
    {
        printf("... WARNING: Material '%s' not found, using default!\n", matname.c_str());
        ospSetObjectAsData(gmodel, "material", OSP_MATERIAL, default_materials[current_renderer_type]);
    }
    
    ospCommit(gmodel);

    if (scene_object == nullptr)
        scene_objects[object_name] = geometry_object;

    scene_instances.push_back(instance);

    return true;
}

bool
update_scene_object(const UpdateObject& update)
{    
    const std::string& object_name = update.name();
    const std::string& linked_data = update.data_link();

    printf("OBJECT '%s' (scene)\n", update.name().c_str());    
    printf("--> '%s'\n", linked_data.c_str());

    SceneObject     *scene_object;
    SceneObjectScene *scene_object_scene;       // XXX yuck

    scene_object = find_scene_object(object_name, SOT_SCENE);

    if (scene_object != nullptr)
    {
        scene_object_scene = dynamic_cast<SceneObjectScene*>(scene_object);
        for (OSPInstance &i : scene_object_scene->instances)
            ospRelease(i);
        scene_object_scene->instances.clear();
        scene_object_scene->lights.clear();
    }
    else
    {
        scene_object_scene = new SceneObjectScene;
        printf("allocating SceneObjectScene %016x\n", scene_object_scene);
        scene_object_scene->data_link = linked_data;
    }

    // Check linked data    
    
    if (!scene_data_with_type_exists(linked_data, SDT_PLUGIN))
    {   
        if (scene_object == nullptr)
            delete scene_object_scene;
        return false;
    }

    PluginInstance* plugin_instance = plugin_instances[linked_data];
    assert(plugin_instance->type == PT_SCENE);
    PluginState *state = plugin_instance->state;

    GroupInstances group_instances = state->group_instances;

    if (group_instances.size() == 0)
        printf("... WARNING: no instances to add!\n");
    else
        printf("... Adding %d instances to scene!\n", group_instances.size());

    glm::mat4   obj2world;
    float       affine_xform[12];

    object2world_from_protobuf(obj2world, update);

    for (GroupInstance& gi : group_instances)
    {
        OSPGroup group = gi.first;
        const glm::mat4 instance_xform = gi.second;

        affine3fv_from_mat4(affine_xform, obj2world * instance_xform);

        OSPInstance instance = ospNewInstance(group);
            ospSetParam(instance, "xfm", OSP_AFFINE3F, affine_xform);
        ospCommit(instance);

        scene_object_scene->instances.push_back(instance);
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
            // This should be coming in 2.0
            scene_object_scene->lights.push_back(light);
            scene_lights.push_back(light);
        }
    }

    if (scene_object == nullptr)
        scene_objects[object_name] = scene_object_scene;

    return true;
}

// XXX has a bug when switching renderer types
bool
update_volume_object(const UpdateObject& update, const Volume& volume_settings)
{
    const std::string& object_name = update.name();
    const std::string& linked_data = update.data_link(); 

    printf("OBJECT '%s' (volume)\n", update.name().c_str()); 
    printf("--> '%s'\n", linked_data.c_str());  

    SceneObject         *scene_object;
    SceneObjectVolume   *volume_object;
    OSPInstance         instance;
    OSPGroup            group;
    OSPVolumetricModel  vmodel;

    scene_object = find_scene_object(object_name, SOT_VOLUME);

    if (scene_object != nullptr)
        volume_object = dynamic_cast<SceneObjectVolume*>(scene_object);
    else
        volume_object = new SceneObjectVolume;

    instance = volume_object->instance;
    assert(instance != nullptr);
    group = volume_object->group;
    assert(group != nullptr); 
    vmodel = volume_object->vmodel;

    // Check linked data
    
    if (!scene_data_with_type_exists(linked_data, SDT_PLUGIN))
    {
        if (scene_object == nullptr)
            delete volume_object;   
        return false;
    }

    PluginInstance* plugin_instance = plugin_instances[linked_data];
    assert(plugin_instance->type == PT_VOLUME);
    PluginState *state = plugin_instance->state;

    OSPVolume volume = state->volume;

    if (volume == NULL)
    {
        printf("... ERROR: volume is NULL!\n");
        delete volume_object;
        return false;
    }    

    if (scene_object == nullptr)
    {
        assert(scene_objects.find(object_name) == scene_objects.end());
        printf("setting %s -> %016x\n", object_name.c_str(), volume_object);
        scene_objects[object_name] = volume_object;
        vmodel = volume_object->vmodel = ospNewVolumetricModel(volume);

        OSPTransferFunction tf = create_transfer_function("cool2warm", state->volume_data_range[0], state->volume_data_range[1]);
        ospSetObject(vmodel, "transferFunction", tf);
        ospRelease(tf);
    }

    // XXX not sure these are handled correctly, and working in API2
    ospSetFloat(vmodel,  "samplingRate", volume_settings.sampling_rate());
    //ospSetFloat(vmodel,  "densityScale", volume_settings.density_scale());  // TODO
    //ospSetFloat(vmodel,  "maxDensity", volume_settings.max_density());  // TODO
    //ospSetFloat(vmodel,  "anisotropy", volume_settings.anisotropy());  // TODO    

    ospCommit(vmodel);

    ospSetObjectAsData(group, "volume", OSP_VOLUMETRIC_MODEL, vmodel);
    ospCommit(group);

    glm::mat4   obj2world;
    float       affine_xform[12];

    object2world_from_protobuf(obj2world, update);
    affine3fv_from_mat4(affine_xform, obj2world);

    ospSetParam(instance, "xfm", OSP_AFFINE3F, affine_xform);
    ospCommit(instance);

    if (scene_object == nullptr)
        scene_objects[object_name] = volume_object;

    scene_instances.push_back(instance);

    return true;
}

bool
update_isosurfaces_object(const UpdateObject& update)
{
    const std::string& object_name = update.name();
    const std::string& linked_data = update.data_link();    

    printf("OBJECT '%s' (isosurfaces)\n", update.name().c_str()); 
    printf("--> '%s'\n", linked_data.c_str());     

    SceneObject         *scene_object;
    SceneObjectIsosurfaces   *isosurfaces_object;
    OSPInstance         instance;
    OSPGroup            group;
    OSPGeometry         isosurfaces_geometry;
    OSPVolumetricModel  vmodel;
    OSPGeometricModel   gmodel;

    scene_object = find_scene_object(object_name, SOT_ISOSURFACES);

    if (scene_object != nullptr)
        isosurfaces_object = dynamic_cast<SceneObjectIsosurfaces*>(scene_object);
    else
        isosurfaces_object = new SceneObjectIsosurfaces;

    instance = isosurfaces_object->instance;
    assert(instance != nullptr);
    group = isosurfaces_object->group;
    assert(group != nullptr); 
    vmodel = isosurfaces_object->vmodel;
    gmodel = isosurfaces_object->gmodel;
    assert(gmodel != nullptr);
    isosurfaces_geometry = isosurfaces_object->isosurfaces_geometry;
    assert(isosurfaces_geometry != nullptr);

    // Check linked data

    if (!scene_data_with_type_exists(linked_data, SDT_PLUGIN))
    {
        if (scene_object != nullptr)
            delete isosurfaces_object;
        return false;
    }

    PluginInstance* plugin_instance = plugin_instances[linked_data];
    assert(plugin_instance->type == PT_VOLUME);
    PluginState *state = plugin_instance->state;

    OSPVolume volume = state->volume;

    if (volume == NULL)
    {
        printf("... ERROR: volume is NULL!\n");
        if (scene_object != nullptr)
            delete isosurfaces_object;
        return false;
    }        

    if (scene_object != nullptr)
    {
        assert(scene_objects.find(object_name) == scene_objects.end());
        printf("setting %s -> %016x\n", object_name.c_str(), isosurfaces_object);
        scene_objects[object_name] = isosurfaces_object;

        // XXX hacked temp volume module
        vmodel = isosurfaces_object->vmodel = ospNewVolumetricModel(volume);
            OSPTransferFunction tf = create_transfer_function("cool2warm", state->volume_data_range[0], state->volume_data_range[1]);
            ospSetObject(vmodel, "transferFunction", tf);
            ospRelease(tf);
            //ospSetFloat(volumeModel, "samplingRate", 0.5f);
         ospCommit(vmodel);

        ospSetObjectAsData(gmodel, "material", OSP_MATERIAL, default_materials[current_renderer_type]);
        ospCommit(gmodel);
     }

    const char *s_custom_properties = update.custom_properties().c_str();
    //printf("Received custom properties:\n%s\n", s_custom_properties);
    const json &custom_properties = json::parse(s_custom_properties);
    printf("... custom properties:\n");
    printf("%s\n", custom_properties.dump(4).c_str());
    
    if (custom_properties.find("isovalues") == custom_properties.end())
    {
        printf("... WARNING: no property 'isovalues' set on object!\n");
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

    OSPData isovalues_data = ospNewData(n, OSP_FLOAT, isovalues, 0);    
    delete [] isovalues;

    ospSetObject(isosurfaces_geometry, "volume", vmodel);       		// XXX structured vol example indicates this needs to be the volume model??
    ospRelease(volume);

    ospSetObject(isosurfaces_geometry, "isovalue", isovalues_data);
    ospRelease(isovalues_data);

    ospCommit(isosurfaces_geometry);

    glm::mat4   obj2world;
    float       affine_xform[12];

    object2world_from_protobuf(obj2world, update);
    affine3fv_from_mat4(affine_xform, obj2world);

    ospSetParam(instance, "xfm", OSP_AFFINE3F, affine_xform);
    ospCommit(instance);

    if (scene_object == nullptr)
        scene_objects[object_name] = isosurfaces_object;

    scene_instances.push_back(instance);
    
    return true;
}

// A slices object is just regular geometry that gets colored 
// using a volume texture
// XXX parenting can be animated using a Childof object constraint
// XXX a text field in a node can't be animated
bool
add_slices_objects(const UpdateObject& update, const Slices& slices)
{
    const std::string& linked_data = update.data_link();

    printf("OBJECT '%s' (slices)\n", update.name().c_str());
    printf("--> '%s'\n", linked_data.c_str());    

    if (!scene_data_with_type_exists(linked_data, SDT_PLUGIN))
        return false;

    PluginInstance* plugin_instance = plugin_instances[linked_data];
    assert(plugin_instance->type == PT_VOLUME);
    PluginState *state = plugin_instance->state;

    OSPVolume volume = state->volume;

    if (volume == NULL)
    {
        printf("... ERROR: volume is NULL!\n");
        return false;
    }        

    const char *s_custom_properties = update.custom_properties().c_str();
    //printf("Received custom properties:\n%s\n", s_custom_properties);
    const json &custom_properties = json::parse(s_custom_properties);
    printf("... custom properties:\n");
    printf("%s\n", custom_properties.dump(4).c_str());

    return true;

#if 0

    SceneObject         *scene_object;
    SceneObjectSlice    *slice_object;
    OSPInstance         instance;
    OSPGroup            group;
    OSPGeometry         isosurfaces_geometry;
    OSPVolumetricModel  vmodel;
    OSPGeometricModel   gmodel;

    // Each slice becomes a separate scene object of type SOT_SLICE
    for (int i = 0; i < slices.slices_size(); i++)
    {
        const Slice& slice = slices.slices(i);

        const std::string& mesh_name = slice.linked_mesh_data();

        scene_object = find_scene_object(object_name, SOT_SLICE);

        if (scene_object != nullptr)
            slices_object = dynamic_cast<SceneObjectSlice*>(scene_object);
        else
            slices_object = new SceneObjectSlices;

        instance = slices_object->instance;
        assert(instance != nullptr);
        group = isosurfaces_object->group;
        assert(group != nullptr); 
        vmodel = isosurfaces_object->vmodel;
        gmodel = isosurfaces_object->gmodel;
        assert(gmodel != nullptr);
        isosurfaces_geometry = isosurfaces_object->isosurfaces_geometry;
        assert(isosurfaces_geometry != nullptr);


        // Get linked geometry

        const std::string& linked_data = slice.linked_mesh();

        printf("... linked mesh '%s' (blender mesh)\n", linked_data.c_str());    

        SceneDataTypeMap::iterator it = scene_data_types.find(linked_data);

        if (it == scene_data_types.end())
        {
            printf("--> '%s' | WARNING: linked data not found!\n", linked_data.c_str());
            return false;
        }
        else if (it->second != SDT_MESH)
        {
            printf("--> '%s' | WARNING: linked data is not of type SDT_MESH but of type %s!\n", 
                linked_data.c_str(), SceneDataType_names[it->second]);
            return false;
        }
        else
            printf("--> '%s' (blender mesh data)\n", linked_data.c_str());

        BlenderMesh *blender_mesh = blender_meshes[linked_data];
        OSPGeometry geometry = blender_mesh->geometry;

        if (geometry == NULL)
        {
            printf("... ERROR: geometry is NULL!\n");
            return false;
        }                    

        // Set up slice geometry

        // XXX temp inserted volumetric model
        auto volume_model = ospNewVolumetricModel(volume);
            OSPTransferFunction tf = create_transfer_function("cool2warm", state->volume_data_range[0], state->volume_data_range[1]);
            ospSetObject(volume_model, "transferFunction", tf);            
            //ospSetFloat(volume_model, "samplingRate", 0.5f);
        ospCommit(volume_model);
        ospRelease(tf);

        OSPTexture volume_texture = ospNewTexture("volume");
            ospSetObject(volume_texture, "volume", volume_model);   // XXX volume model, not volume
        ospCommit(volume_texture);

        OSPMaterial material = ospNewMaterial(current_renderer_type.c_str(), "default");
            ospSetObject(material, "map_Kd", volume_texture);
        ospCommit(material);
        ospRelease(volume_texture);        

        OSPGeometricModel geometric_model = ospNewGeometricModel(geometry);
            ospSetObjectAsData(geometric_model, "material", OSP_MATERIAL, material);
        ospCommit(geometric_model);
        ospRelease(material);

        OSPGroup group = ospNewGroup();
            ospSetObjectAsData(group, "geometry", OSP_GEOMETRIC_MODEL, geometric_model); 
            //ospRelease(model);
        ospCommit(group);
     
        glm::mat4   obj2world;
        float       affine_xform[12];

        object2world_from_protobuf(obj2world, slice);
        affine3fv_from_mat4(affine_xform, obj2world);

        OSPInstance instance = ospNewInstance(group);
            ospSetAffine3fv(instance, "xfm", affine_xform);
        ospCommit(instance);
        ospRelease(group);

        //if (scene_object == nullptr)
        //scene_objects[object_name] = ...

        scene_instances.push_back(instance);

#if 0
        plane[0] = slice.a();
        plane[1] = slice.b();
        plane[2] = slice.c();
        plane[3] = slice.d();

        printf("... plane[%d]: %.3f, %3f, %.3f, %.3f\n", i, plane[0], plane[1], plane[2], plane[3]);

        OSPData planeData = ospNewData(1, OSP_VEC4F, plane, 0);        

            // XXX hacked temp volume module
        auto volumeModel = ospNewVolumetricModel(volume);
            OSPTransferFunction tf = create_transfer_function("cool2warm", state->volume_data_range[0], state->volume_data_range[1]);
            ospSetObject(volumeModel, "transferFunction", tf);
            ospRelease(tf);
        ospCommit(volumeModel);

        OSPGeometry slice_geometry = ospNewGeometry("slices");
            ospSetObject(slice_geometry, "volume", volumeModel);         // XXX volume model, not volume
            //ospRelease(volumeModel);
            ospSetObject(slice_geometry, "plane", planeData);
            ospRelease(planeData);
        ospCommit(slice_geometry);
            
        OSPGeometricModel model = ospNewGeometricModel(slice_geometry);
            ospSetObjectAsData(model, "material", OSP_MATERIAL, default_material);
        ospCommit(model);
        ospRelease(slice_geometry);
#endif        
    }

#endif
    return true;
}

bool
update_light_object(const UpdateObject& update, const LightSettings& light_settings)
{
    const std::string& object_name = light_settings.object_name();
    const std::string& linked_data = light_settings.light_name();    

    printf("OBJECT '%s' (light)\n", object_name.c_str());
    //printf("--> '%s' (blender light data)\n", linked_data.c_str());    // XXX not set for ambient

    SceneObject *scene_object;
    SceneObjectLight *light_object = nullptr;
    OSPLight light;
    LightSettings::Type light_type;
    bool create_new_light = false;

    scene_object = find_scene_object(object_name, SOT_LIGHT);

    if (scene_object != nullptr)
    {
        light_object = dynamic_cast<SceneObjectLight*>(scene_object);
        light = light_object->light;
        assert(light != nullptr);
        light_type = light_object->light_type;
        if (light_type != light_settings.type())
        {            
            printf("... Light type changed from %d to %d, replacing with new light\n",
                light_type, light_settings.type());
            delete_object(object_name);
            light_object = nullptr;
        }
    }
    
    if (light_object == nullptr)
    {
        light_type = light_settings.type();
        light_object = new SceneObjectLight;
        switch (light_type)
        {
        /*case LightSettings::AMBIENT:
            light = ospNewLight("ambient");
            break;*/
        case LightSettings::POINT:
            light = ospNewLight("sphere");
            break; 
        case LightSettings::SPOT:
            light = ospNewLight("spot");
            break; 
        case LightSettings::SUN:
            light = ospNewLight("distant");
            break; 
        case  LightSettings::AREA:
            light = ospNewLight("quad");
            break; 
        default:
            printf("ERROR: unhandled light type %d!\n", light_type);
        }

        light_object->light = light;
        light_object->light_type = light_type;
        light_object->data_link = light_settings.light_name();

        scene_objects[object_name] = light_object;        
    }

    if (light_settings.type() == LightSettings::SPOT)
    {
        ospSetFloat(light, "openingAngle", light_settings.opening_angle());
        ospSetFloat(light, "penumbraAngle", light_settings.penumbra_angle());
    }
    else if (light_settings.type() == LightSettings::SUN)
    {
        ospSetFloat(light, "angularDiameter", light_settings.angular_diameter());
    }
    else if (light_settings.type() == LightSettings::AREA)
    {
        // XXX blender's area light is more general than ospray's quad light
        ospSetVec3f(light, "edge1", light_settings.edge1(0), light_settings.edge1(1), light_settings.edge1(2));
        ospSetVec3f(light, "edge2", light_settings.edge2(0), light_settings.edge2(1), light_settings.edge2(2));
    }
    //else
    // XXX HDRI

    printf("... intensity %.3f, visible %d\n", light_settings.intensity(), light_settings.visible());

    ospSetVec3f(light, "color", light_settings.color(0), light_settings.color(1), light_settings.color(2));
    ospSetFloat(light, "intensity", light_settings.intensity());
    ospSetBool(light, "visible", light_settings.visible());

    if (light_settings.type() != LightSettings::SUN && light_settings.type() != LightSettings::AMBIENT)
        ospSetVec3f(light, "position", light_settings.position(0), light_settings.position(1), light_settings.position(2));

    if (light_settings.type() == LightSettings::SUN || light_settings.type() == LightSettings::SPOT)
        ospSetVec3f(light, "direction", light_settings.direction(0), light_settings.direction(1), light_settings.direction(2));

    if (light_settings.type() == LightSettings::POINT || light_settings.type() == LightSettings::SPOT)
        ospSetFloat(light, "radius", light_settings.radius());

    ospCommit(light);  

    scene_lights.push_back(light);  

    return true;
}

// XXX add world/object bounds
bool 
handle_get_server_state(TCPSocket *sock)
{    
    json j, p;

    p = {};
    for (auto& kv: scene_objects)
    {
        const SceneObject* object = kv.second;
        p[kv.first] = { {"type", SceneObjectType_names[object->type]}, {"data_link", object->data_link} };
    }
    j["scene_objects"] = p;

    p = {};
    for (auto& kv: scene_materials)
        p[kv.first] = (size_t)kv.second;
    j["scene_materials"] = p;        

    p = {};
    for (auto& kv: plugin_instances)
    {
        const PluginInstance *instance = kv.second;
        const PluginState *state = instance->state;

        json ll;
        for (auto& l : state->lights)
            ll.push_back((size_t)l);

        json gi;
        for (auto& i : state->group_instances)
            gi.push_back({(size_t)(i.first), to_string(i.second)});

        json d = p[kv.first] = { 
            {"name", instance->name}, 
            {"type", PluginType_names[instance->type]},
            {"plugin_name", instance->plugin_name},
            {"parameters_hash", instance->parameters_hash},
            {"custom_properties_hash", instance->custom_properties_hash},
            {"state", {
                {"renderer", state->renderer},
                {"uses_renderer_type", state->uses_renderer_type},
                {"parameters", state->parameters},
                {"bound", (size_t)state->bound},
                {"geometry", (size_t)state->geometry},
                {"volume", (size_t)state->volume},
                {"volume_data_range", { state->volume_data_range[0], state->volume_data_range[1] } },
                {"data", (size_t)state->data},
                {"lights", ll},
                {"group_instances", gi}
            } }
        };
    }
    j["plugin_instances"] = p;

    p = {};
    for (auto& kv: blender_meshes)
    {
        const BlenderMesh *mesh = kv.second;
        p[kv.first] = { 
            {"name", mesh->name}, {"parameters", mesh->parameters}, {"geometry", (size_t)mesh->geometry},
            {"num_vertices", mesh->num_vertices}, {"num_triangles", mesh->num_triangles}
        };
    }
    j["blender_meshes"] = p;

    p = {};
    for (auto& kv: scene_data_types)
    {
        p[kv.first] = SceneDataType_names[kv.second];
    }
    j["scene_data_types"] = p;

    p = {};
    for (auto& kv: plugin_definitions)
    {
        const PluginDefinition& pdef = kv.second;
        p[kv.first] = { {"type", PluginType_names[pdef.type]}, {"uses_renderer_type", pdef.uses_renderer_type} };    // XXX params
    }
    j["plugin_definitions"] = p;

    // Scene 

    json scene;

    p = {};
    for (auto& i: scene_instances)
        p.push_back((size_t)i);
    scene["scene_instances"] = p;

    p = {};
    for (auto& l: scene_lights)
        p.push_back((size_t)l);
    scene["scene_lights"] = p;

    j["scene"] = scene;

    // Send result

    ServerStateResult   result;

    result.set_state(j.dump(4));

    send_protobuf(sock, result);

    return true;
}

bool
handle_update_object(TCPSocket *sock)
{
    UpdateObject    update;    

    if (!receive_protobuf(sock, update))
        return false;

    //print_protobuf(update);

    switch (update.type())
    {
    case UpdateObject::MESH:
        update_blender_mesh_object(update);
        break;

    case UpdateObject::GEOMETRY:
        update_geometry_object(update);
        break;

    case UpdateObject::SCENE:
        update_scene_object(update);
        break;

    case UpdateObject::VOLUME:
        {
        Volume volume;
        if (!receive_protobuf(sock, volume))
            return false;
        update_volume_object(update, volume);
        }
        break;

    case UpdateObject::ISOSURFACES:
        update_isosurfaces_object(update);
        break;
    
    case UpdateObject::SLICES:
        {
        Slices slices;
        if (!receive_protobuf(sock, slices))
            return false;
        add_slices_objects(update, slices);
        }
        break;

    case UpdateObject::LIGHT:
        {
        LightSettings light_settings;
        if (!receive_protobuf(sock, light_settings))
            return false;
        update_light_object(update, light_settings);
        }
        break;

    default:
        printf("WARNING: unhandled update type %s\n", UpdateObject_Type_descriptor()->FindValueByNumber(update.type())->name().c_str());
        break;
    }

    return true;
}

void
update_framebuffer(OSPFrameBufferFormat format, uint32_t width, uint32_t height)
{
    printf("FRAMEBUFFER %d x %d (format %d)\n", width, height, format);

    if (framebuffer_width != width || framebuffer_height != height || framebuffer_format != format)
    {
        // Reallocate framebuffer as its resolution/format changed
        if (framebuffer_created)
            ospRelease(framebuffer);

        framebuffer_width = width;
        framebuffer_height = height;

        printf("Initializing framebuffer of %dx%d pixels\n", framebuffer_width, framebuffer_height);

        // OSP_FB_SRGBA   : 8 bit sRGB gamma encoded color components, and linear alpha
        // OSP_FB_RGBA32F : 32 bit float components red, green, blue, alpha
        framebuffer = ospNewFrameBuffer(framebuffer_width, framebuffer_height, format, OSP_FB_COLOR | /*OSP_FB_DEPTH |*/ OSP_FB_ACCUM | OSP_FB_VARIANCE);
        // XXX is this call needed here?
        ospResetAccumulation(framebuffer);

        framebuffer_created = true;
    }
}

void
update_camera(CameraSettings& camera_settings)
{
    printf("CAMERA '%s' (camera)\n", camera_settings.object_name().c_str());
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
    
    // XXX for now create new cam object
    if (camera != nullptr)
        ospRelease(camera);

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

    ospSetParam(camera, "position", OSP_VEC3F, cam_pos);
    ospSetParam(camera, "direction", OSP_VEC3F, cam_viewdir);
    ospSetParam(camera, "up",  OSP_VEC3F, cam_updir);

    if (camera_settings.dof_focus_distance() > 0.0f)
    {
        // XXX seem to stuck in loop during rendering when distance is 0
        ospSetFloat(camera, "focusDistance", camera_settings.dof_focus_distance());
        ospSetFloat(camera, "apertureRadius", camera_settings.dof_aperture());
    }

    if (camera_settings.border_size() == 4)
    {
        // Border render enabled
        ospSetVec2f(camera, "imageStart", camera_settings.border(0), camera_settings.border(1));
        ospSetVec2f(camera, "imageEnd", camera_settings.border(2), camera_settings.border(3));
    }    

    ospCommit(camera);
}

void
handle_update_material(TCPSocket *sock)
{
    MaterialUpdate update;

    receive_protobuf(sock, update);

    printf("MATERIAL '%s'\n", update.name().c_str());

    SceneMaterial *scene_material = nullptr;
    OSPMaterial material = nullptr;

    SceneMaterialMap::iterator it = scene_materials.find(update.name());
    if (it != scene_materials.end())
    {
        printf("... Updating existing material\n");

        scene_material = it->second;
        if (scene_material->type != update.type())
        {
            printf("... Material type changed\n");
            delete scene_material;
            scene_material = nullptr;
            scene_materials.erase(update.name());
        }
        else
            material = scene_material->material;
    }

    switch (update.type())
    {

    case MaterialUpdate::CAR_PAINT:
    {
        CarPaintSettings settings;

        receive_protobuf(sock, settings);
        printf("... Car paint\n");

        if (scene_material == nullptr)
        {
            scene_material = new SceneMaterial;
            material = scene_material->material = ospNewMaterial(current_renderer_type.c_str(), "CarPaint");
        }

        if (settings.base_color_size() == 3)
            ospSetVec3f(material, "baseColor", settings.base_color(0), settings.base_color(1), settings.base_color(2));    
        ospSetFloat(material, "roughness", settings.roughness());
        ospSetFloat(material, "normal", settings.normal());
        ospSetFloat(material, "flakeDensity", settings.flake_density());    
        ospSetFloat(material, "flakeScale", settings.flake_scale());    
        ospSetFloat(material, "flakeSpread", settings.flake_spread());    
        ospSetFloat(material, "flakeJitter", settings.flake_jitter());    
        ospSetFloat(material, "flakeRoughness", settings.flake_roughness());
        ospSetFloat(material, "coat", settings.coat());
        ospSetFloat(material, "coatIor", settings.coat_ior());
        if (settings.coat_color_size() == 3)
            ospSetVec3f(material, "coatColor", settings.coat_color(0), settings.coat_color(1), settings.coat_color(2)); 
        ospSetFloat(material, "coatThickness", settings.coat_thickness());
        ospSetFloat(material, "coatRoughness", settings.coat_roughness());
        ospSetFloat(material, "coatNormal", settings.coat_normal());
        if (settings.flipflop_color_size() == 3)
            ospSetVec3f(material, "flipflopColor", settings.flipflop_color(0), settings.flipflop_color(1), settings.flipflop_color(2)); 
        ospSetFloat(material, "flipflopFalloff", settings.flipflop_falloff());

        break;        
    }

    case MaterialUpdate::GLASS:
    {
        GlassSettings settings;

        receive_protobuf(sock, settings);
        printf("... Glass\n");

        if (scene_material == nullptr)
        {
            scene_material = new SceneMaterial;
            material = scene_material->material = ospNewMaterial(current_renderer_type.c_str(), "Glass");
        }

        ospSetFloat(material, "eta", settings.eta());
        if (settings.attenuation_color_size() == 3)
            ospSetVec3f(material, "attenuationColor", settings.attenuation_color(0), settings.attenuation_color(1), settings.attenuation_color(2));        
        ospSetFloat(material, "attenuationDistance", settings.attenuation_distance());

        break;
    }

    case MaterialUpdate::LUMINOUS:
    {
        LuminousSettings settings;

        receive_protobuf(sock, settings);
        printf("... Luminous\n");

        if (scene_material == nullptr)
        {
            scene_material = new SceneMaterial;
            material = scene_material->material = ospNewMaterial(current_renderer_type.c_str(), "Glass");
        }

        if (settings.color_size() == 3)
            ospSetVec3f(material, "color", settings.color(0), settings.color(1), settings.color(2));    
        ospSetFloat(material, "intensity", settings.intensity());    
        ospSetFloat(material, "transparency", settings.transparency());

        break;
    }

    case MaterialUpdate::METALLIC_PAINT:
    {
        MetallicPaintSettings settings;

        receive_protobuf(sock, settings);
        printf("... MetallicPaint\n");

        if (scene_material == nullptr)
        {
            scene_material = new SceneMaterial;
            material = scene_material->material = ospNewMaterial(current_renderer_type.c_str(), "MetallicPaint");
        }

        if (settings.base_color_size() == 3)
            ospSetVec3f(material, "baseColor", settings.base_color(0), settings.base_color(1), settings.base_color(2));    
        if (settings.flake_color_size() == 3)
            ospSetVec3f(material, "flakeColor", settings.flake_color(0), settings.flake_color(1), settings.flake_color(2));   
        ospSetFloat(material, "flakeAmount", settings.flake_amount());    
        ospSetFloat(material, "flakeSpread", settings.flake_spread());    
        ospSetFloat(material, "eta", settings.eta());

        break;
    }

    case MaterialUpdate::OBJMATERIAL:
    {
        OBJMaterialSettings settings;

        receive_protobuf(sock, settings);
        printf("... OBJMaterial (Kd %.3f,%.3f,%.3f; ...)\n", settings.kd(0), settings.kd(1), settings.kd(2));

        if (scene_material == nullptr)
        {
            scene_material = new SceneMaterial;
            material = scene_material->material = ospNewMaterial(current_renderer_type.c_str(), "OBJMaterial");
        }

        if (settings.kd_size() == 3)
            ospSetVec3f(material, "Kd", settings.kd(0), settings.kd(1), settings.kd(2));
        if (settings.ks_size() == 3)
            ospSetVec3f(material, "Ks", settings.ks(0), settings.ks(1), settings.ks(2));
        ospSetFloat(material, "Ns", settings.ns());
        ospSetFloat(material, "d", settings.d());            

        break;
    }

    case MaterialUpdate::PRINCIPLED:
    {
        PrincipledSettings settings;

        receive_protobuf(sock, settings);
        printf("... Principled\n");

        if (scene_material == nullptr)
        {
            scene_material = new SceneMaterial;
            material = scene_material->material = ospNewMaterial(current_renderer_type.c_str(), "Principled");
        }

        if (settings.base_color_size() == 3)
            ospSetVec3f(material, "baseColor", settings.base_color(0), settings.base_color(1), settings.base_color(2));    
        if (settings.edge_color_size() == 3)
            ospSetVec3f(material, "edgeColor", settings.edge_color(0), settings.edge_color(1), settings.edge_color(2)); 
        ospSetFloat(material, "metallic", settings.metallic());    
        ospSetFloat(material, "diffuse", settings.diffuse());    
        ospSetFloat(material, "specular", settings.specular());
        ospSetFloat(material, "ior", settings.ior());
        ospSetFloat(material, "transmission", settings.transmission());
        if (settings.transmission_color_size() == 3)
            ospSetVec3f(material, "transmissionColor", settings.transmission_color(0), settings.transmission_color(1), settings.transmission_color(2)); 
        ospSetFloat(material, "transmissionDepth", settings.transmission_depth());
        ospSetFloat(material, "roughness", settings.roughness());
        ospSetFloat(material, "anisotropy", settings.anisotropy());
        ospSetFloat(material, "rotation", settings.rotation());
        ospSetFloat(material, "normal", settings.normal());
        ospSetFloat(material, "baseNormal", settings.base_normal());
        ospSetBool(material, "thin", settings.thin());
        ospSetFloat(material, "thickness", settings.thickness());
        ospSetFloat(material, "backlight", settings.backlight());
        ospSetFloat(material, "coat", settings.coat());
        ospSetFloat(material, "coatIor", settings.coat_ior());
        if (settings.coat_color_size() == 3)
            ospSetVec3f(material, "coatColor", settings.coat_color(0), settings.coat_color(1), settings.coat_color(2)); 
        ospSetFloat(material, "coatThickness", settings.coat_thickness());
        ospSetFloat(material, "coatRoughness", settings.coat_roughness());
        ospSetFloat(material, "coatNormal", settings.coat_normal());
        ospSetFloat(material, "sheen", settings.sheen());
        if (settings.sheen_color_size() == 3)
            ospSetVec3f(material, "sheenColor", settings.sheen_color(0), settings.sheen_color(1), settings.sheen_color(2)); 
        ospSetFloat(material, "sheenTint", settings.sheen_tint());
        ospSetFloat(material, "sheenRoughness", settings.sheen_roughness());
        ospSetFloat(material, "opacity", settings.opacity());

        break;
    }

    default:
        printf("ERROR: unknown material update type %d!\n", update.type());

    }

    scene_material->type = update.type();
    scene_materials[update.name()] = scene_material;

    ospCommit(material);
}

void
update_renderer_type(const std::string& type)
{
    if (type == current_renderer_type)
        return;

    printf("Updating renderer type to '%s'\n", type.c_str());

    renderer = renderers[type.c_str()];

    scene_materials.clear();
    // XXX any more?

    current_renderer_type = type;
}

bool
update_render_settings(const RenderSettings& render_settings)
{
    printf("Applying render settings\n");

    render_samples = render_settings.samples();
    //ospSetInt(renderer, "spp", 1);

    ospSetInt(renderer, "maxDepth", render_settings.max_depth());
    ospSetFloat(renderer, "minContribution", render_settings.min_contribution());
    ospSetFloat(renderer, "varianceThreshold", render_settings.variance_threshold());

    if (current_renderer_type == "scivis")
    {
        ospSetInt(renderer, "aoSamples", render_settings.ao_samples());
        ospSetFloat(renderer, "aoRadius", render_settings.ao_radius());
        ospSetFloat(renderer, "aoIntensity", render_settings.ao_intensity());
    }
    else
    {
        // Pathtracer

        ospSetInt(renderer, "rouletteDepth", render_settings.roulette_depth());
        ospSetFloat(renderer, "maxContribution", render_settings.max_contribution());
        ospSetBool(renderer, "geometryLights", render_settings.geometry_lights());
    }

    ospCommit(renderer);

    // Done!

    return true;
}

bool
update_world_settings(const WorldSettings& world_settings)
{    
    printf("Updating world settings\n");

    printf("... ambient color %.3f, %.3f, %.3f; intensity %.3f\n", 
        world_settings.ambient_color(0), 
        world_settings.ambient_color(1), 
        world_settings.ambient_color(2), 
        world_settings.ambient_intensity());

    ospSetVec3f(ambient_light, "color", world_settings.ambient_color(0), world_settings.ambient_color(1), world_settings.ambient_color(2));
    ospSetFloat(ambient_light, "intensity", world_settings.ambient_intensity());
    ospCommit(ambient_light);

    printf("... background color %f, %f, %f, %f\n", 
        world_settings.background_color(0),
        world_settings.background_color(1),
        world_settings.background_color(2),
        world_settings.background_color(3));    

    if (current_renderer_type == "scivis")
    {
        ospSetVec4f(renderer, "bgColor",
            world_settings.background_color(0),
            world_settings.background_color(1),
            world_settings.background_color(2),
            world_settings.background_color(3));
    }
    else
    {
        // Pathtracer

        // Work around unsupported bgColor
        // https://github.com/ospray/ospray/issues/347

        float texel[4] = { 
            world_settings.background_color(0),
            world_settings.background_color(1),
            world_settings.background_color(2),
            world_settings.background_color(3)
        };

        OSPData data = ospNewData(1, OSP_VEC4F, texel, 0);

        OSPTexture backplate = ospNewTexture("texture2d");    
            ospSetInt(backplate, "format", OSP_TEXTURE_RGBA32F);
            ospSetVec2i(backplate, "size", 1, 1);            
            ospSetObject(backplate, "data", data);
        ospCommit(backplate);            
        ospRelease(data);

        ospSetObject(renderer, "backplate", backplate);
        ospRelease(backplate);
    }

    ospCommit(renderer);

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
    float variance;
    int num_samples;
    float mem_usage, peak_memory_usage=0.0f;

    gettimeofday(&t0, NULL);

    // Clear framebuffer
    // XXX no 2.0 equivalent?
    //ospFrameBufferClear(framebuffer, OSP_FB_COLOR | OSP_FB_ACCUM);
    ospResetAccumulation(framebuffer);

    printf("Rendering %d samples:\n", render_samples);

    for (int i = 1; i <= render_samples; i++)
    {
        printf("[%d/%d] ", i, render_samples);
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
        // XXX use new future-based call?
        variance = ospRenderFrameBlocking(framebuffer, renderer, camera, world);

        gettimeofday(&t2, NULL);
        printf("%.3f seconds (variance %.3f)\n", time_diff(t1, t2), variance);
        
        // Check for cancel before writing framebuffer to file
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

        // Save framebuffer to file
        sprintf(fname, "/dev/shm/blosprayfb%04d.exr", i);

        framebuffer_file_size = write_framebuffer_exr(fname);
        // XXX check result value

        mem_usage = memory_usage();
        peak_memory_usage = std::max(mem_usage, peak_memory_usage);

        // Signal a new frame is available
        RenderResult rs;
        rs.set_type(RenderResult::FRAME);
        rs.set_sample(i);
        rs.set_variance(variance);
        rs.set_file_name(fname);
        rs.set_file_size(framebuffer_file_size);
        rs.set_memory_usage(mem_usage);
        rs.set_peak_memory_usage(peak_memory_usage);

        render_result_queue.push(rs);
    }

    mem_usage = memory_usage();
    peak_memory_usage = std::max(mem_usage, peak_memory_usage);

    // Final frame
    RenderResult rs;
    rs.set_type(RenderResult::DONE);    
    rs.set_variance(variance);
    rs.set_memory_usage(mem_usage);
    rs.set_peak_memory_usage(peak_memory_usage);
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
clear_scene()
{
    printf("Clearing scene (OSPRay elements only)\n");

    ospRelease(scene_instances_data);
    ospRelease(scene_lights_data);    

    scene_instances.clear();
    scene_instances_data = nullptr;

    scene_lights.clear();
    scene_lights.push_back(ambient_light);
    scene_lights_data = nullptr;

    if (world != nullptr)
        ospRelease(world);

    return true;
}

bool
prepare_scene()
{
    // XXX might not have to recreate world, only update instances
    world = ospNewWorld();
    // Check https://github.com/ospray/ospray/issues/277. Is bool setting fixed in 2.0?
    //ospSetBool(world, "compactMode", true);

    printf("Setting up world with %d instance(s)\n", scene_instances.size());
    if (scene_instances.size() > 0)
    {
        ospRelease(scene_instances_data);    
        scene_instances_data = ospNewSharedData(&scene_instances[0], OSP_INSTANCE, scene_instances.size());
        ospSetObject(world, "instance", scene_instances_data);    
        ospRetain(scene_instances_data);
    }
    
    printf("Adding %d light(s) to the world\n", scene_lights.size());
    if (scene_lights.size() > 0)
    {
        ospRelease(scene_lights_data);
        scene_lights_data = ospNewSharedData(&scene_lights[0], OSP_LIGHT, scene_lights.size());
        ospSetObject(world, "light", scene_lights_data);
        ospRetain(scene_lights_data);
    }

    ospCommit(world);

    return true;
}

bool 
handle_hello(TCPSocket *sock, const ClientMessage& client_message)
{
    const uint32_t client_version = client_message.uint_value();    

    HelloResult result;
    bool res = true;

    if (client_version != PROTOCOL_VERSION)
    {
        char s[256];
        sprintf(s, "Client protocol version %d does not match our protocol version %d", client_version, PROTOCOL_VERSION);
        printf("ERROR: %s\n", s);

        result.set_success(false);
        result.set_message(s);
        res = false;  
    } 
    else
    {
        //printf("Got HELLO message, client protocol version %d matches ours\n", client_version);
        result.set_success(true);
    }

    send_protobuf(sock, result);

    return res;
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

    CameraSettings      camera_settings;
    RenderSettings      render_settings;
    WorldSettings       world_settings;

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

            if (dump_client_messages)
            {
                printf("Got client message of type %s\n", ClientMessage_Type_Name(client_message.type()).c_str());
                printf("%s\n", client_message.DebugString().c_str());
            }

            switch (client_message.type())
            {
                case ClientMessage::HELLO:
                    if (!handle_hello(sock, client_message))
                    {
                        sock->close();
                        return false;
                    }

                    break;

                case ClientMessage::BYE:
                    // XXX if we were still rendering, handle the chaos
                    printf("Got BYE message\n");
                    sock->close();
                    return true;

                case ClientMessage::QUIT:
                    // XXX if we were still rendering, handle the chaos
                    // XXX exit server
                    printf("Got QUIT message\n");
                    sock->close();
                    return true;

                case ClientMessage::UPDATE_RENDERER_TYPE:
                    update_renderer_type(client_message.string_value());
                    break;

                case ClientMessage::CLEAR_SCENE:
                    clear_scene();
                    break;

                case ClientMessage::UPDATE_RENDER_SETTINGS:     
                    if (!receive_protobuf(sock, render_settings))
                    {
                        sock->close();
                        return false;
                    }
                    update_render_settings(render_settings);
                    break;

                case ClientMessage::UPDATE_WORLD_SETTINGS:
                    if (!receive_protobuf(sock, world_settings))
                    {
                        sock->close();
                        return false;
                    }
                    update_world_settings(world_settings);
                    break;

                case ClientMessage::UPDATE_PLUGIN_INSTANCE:
                    handle_update_plugin_instance(sock);
                    break;

                case ClientMessage::UPDATE_BLENDER_MESH:
                    handle_update_blender_mesh_data(sock, client_message.string_value());
                    break;

                case ClientMessage::UPDATE_OBJECT:
                    handle_update_object(sock);
                    break;
                
                case ClientMessage::UPDATE_FRAMEBUFFER:
                    update_framebuffer((OSPFrameBufferFormat)(client_message.uint_value()), 
                        client_message.uint_value2(), client_message.uint_value3());
                    break;

                case ClientMessage::UPDATE_CAMERA:
                    if (!receive_protobuf(sock, camera_settings))
                    {
                        sock->close();
                        return false;
                    }
                    update_camera(camera_settings);
                    break;

                case ClientMessage::UPDATE_MATERIAL:
                    handle_update_material(sock);
                    break;

                case ClientMessage::GET_SERVER_STATE:
                    handle_get_server_state(sock);
                    break;

                case ClientMessage::QUERY_BOUND:
                    handle_query_bound(sock, client_message.string_value());
                    break;

                case ClientMessage::START_RENDERING:

                    if (rendering)
                    {
                        // Ignore                        
                        printf("Received ClientMessage::START_RENDERING, but we're already rendering!\n");                        
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

                default:
                    printf("WARNING: unhandled client message %d!\n", client_message.type());
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

                    //printf("Frame available, sample %d (%s, %d bytes)\n", render_result.sample(), render_result.file_name().c_str(), render_result.file_size());

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

void
prepare_renderers()
{
    OSPMaterial m;

    renderers["scivis"] = ospNewRenderer("scivis");
    renderers["pathtracer"] = ospNewRenderer("pathtracer");

    m = default_materials["scivis"] = ospNewMaterial("scivis", "OBJMaterial");
       ospSetVec3f(m, "Kd", 0.8f, 0.8f, 0.8f);
    ospCommit(m);

    m = default_materials["pathtracer"] = ospNewMaterial("pathtracer", "OBJMaterial");
        ospSetVec3f(m, "Kd", 0.8f, 0.8f, 0.8f);
    ospCommit(m);

    // XXX move somewhere else
    ambient_light = ospNewLight("ambient");
}

// Error/status display

void
ospray_error(OSPError e, const char *error)
{
    printf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    printf("OSPRAY ERROR: %s\n", error);
    printf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");

    if (abort_on_ospray_error)
        abort();
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
    OSPError init_error = ospInit(&argc, argv);
    if (init_error != OSP_NO_ERROR)
    {
        printf("Error initializing OSPRay: %d\n", init_error);
        exit(-1);
    }

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
        //printf("Waiting for new connection...\n");

        sock = listen_sock->accept();

        printf("---------------------------------------------------------------\n");
        printf("Got new connection\n");

        if (!handle_connection(sock))
            printf("Error handling connection!\n");
        //else
        //    printf("Connection successfully handled\n");
    }

    return 0;
}
