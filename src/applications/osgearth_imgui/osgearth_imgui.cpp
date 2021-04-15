/* -*-c++-*- */
/* osgEarth - Geospatial SDK for OpenSceneGraph
* Copyright 2020 Pelican Mapping
* http://osgearth.org
*
* osgEarth is free software; you can redistribute it and/or modify
* it under the terms of the GNU Lesser General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
* IN THE SOFTWARE.
*
* You should have received a copy of the GNU Lesser General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>
*/

#include <osgEarth/ImGui/ImGui>
#include <osgEarth/EarthManipulator>
#include <osgEarth/ExampleResources>
#include <osgViewer/Viewer>

#define LC "[imgui] "

using namespace osgEarth;
using namespace osgEarth::Util;

struct AppGUI : public GUI::DemoGUI
{
    AppGUI(osg::Node* node) : GUI::DemoGUI()
    {
        setVisible(typeid(GUI::LayersGUI), true);
        setVisible(typeid(GUI::ViewpointsGUI), true);
        setVisible(typeid(GUI::SystemGUI), true);
        setVisible(typeid(GUI::EphemerisGUI), true);
    }
};

int
usage(const char* name)
{
    OE_NOTICE
        << "\nUsage: " << name << " file.earth" << std::endl
        << MapNodeHelper().usage() << std::endl;
    return 0;
}

int
main(int argc, char** argv)
{
    osg::ArgumentParser arguments(&argc, argv);
    if (arguments.read("--help"))
        return usage(argv[0]);

    osgEarth::initialize();

    osgViewer::Viewer viewer(arguments);
    viewer.setCameraManipulator(new EarthManipulator(arguments));

    // Call this to enable ImGui rendering:
    viewer.setRealizeOperation(new AppGUI::RealizeOperation);

    osg::Node* node = MapNodeHelper().loadWithoutControls(arguments, &viewer);
    if (node)
    {
        // Call this to add the ImGui GUI panels:
        viewer.addEventHandler(new AppGUI(node));

        viewer.setSceneData(node);
        return viewer.run();
    }
    else
    {
        return usage(argv[0]);
    }
}
