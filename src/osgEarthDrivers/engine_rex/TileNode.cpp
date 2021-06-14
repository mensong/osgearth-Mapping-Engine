/* -*-c++-*- */
/* osgEarth - Geospatial SDK for OpenSceneGraph
* Copyright 2008-2014 Pelican Mapping
* http://osgearth.org
*
* osgEarth is free software; you can redistribute it and/or modify
* it under the terms of the GNU Lesser General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>
*/
#include "TileNode"
#include "SurfaceNode"
#include "ProxySurfaceNode"
#include "EngineContext"
#include "Loader"
#include "LoadTileData"
#include "SelectionInfo"
#include "ElevationTextureUtils"
#include "TerrainCuller"
#include "RexTerrainEngineNode"

#include <osgEarth/CullingUtils>
#include <osgEarth/ImageUtils>
#include <osgEarth/Utils>
#include <osgEarth/NodeUtils>
#include <osgEarth/Metrics>
#include <osg/TriangleFunctor>

using namespace osgEarth::REX;
using namespace osgEarth;
using namespace osgEarth::Util;

#define LC "[TileNode] "

namespace
{
    // Scale and bias matrices, one for each TileKey quadrant.
    const osg::Matrixf scaleBias[4] =
    {
        osg::Matrixf(0.5f,0,0,0, 0,0.5f,0,0, 0,0,1.0f,0, 0.0f,0.5f,0,1.0f),
        osg::Matrixf(0.5f,0,0,0, 0,0.5f,0,0, 0,0,1.0f,0, 0.5f,0.5f,0,1.0f),
        osg::Matrixf(0.5f,0,0,0, 0,0.5f,0,0, 0,0,1.0f,0, 0.0f,0.0f,0,1.0f),
        osg::Matrixf(0.5f,0,0,0, 0,0.5f,0,0, 0,0,1.0f,0, 0.5f,0.0f,0,1.0f)
    };
}

TileNode::TileNode(
    const TileKey& key,
    TileNode* parent,
    EngineContext* context,
    Cancelable* progress) :

    _key(key),
    _context(context),
    _loadsInQueue(0u),
    _childrenReady(false),
    _lastTraversalTime(0.0),
    _lastTraversalFrame(0),
    _empty(false), // an "empty" node exists but has no geometry or children
    _imageUpdatesActive(false),
    _doNotExpire(false),
    _revision(0),
    _mutex("TileNode(OE)"),
    _loadQueue("TileNode LoadQueue(OE)"),
    _createChildAsync(true),
    _nextLoadManifestPtr(nullptr),
    _loadPriority(0.0f)
{
    OE_HARD_ASSERT(context != nullptr, __func__);

    // build the actual geometry for this node
    createGeometry(progress);

    // Encode the tile key in a uniform. Note! The X and Y components are presented
    // modulo 2^16 form so they don't overrun single-precision space.
    unsigned tw, th;
    _key.getProfile()->getNumTiles(_key.getLOD(), tw, th);

    const double m = 65536; //pow(2.0, 16.0);

    double x = (double)_key.getTileX();
    double y = (double)(th - _key.getTileY() - 1);

    _tileKeyValue.set(
        (float)fmod(x, m),
        (float)fmod(y, m),
        (float)_key.getLOD(),
        -1.0f);

    // initialize all the per-tile uniforms the shaders will need:
    float range, morphStart, morphEnd;
    context->getSelectionInfo().get(_key, range, morphStart, morphEnd);

    float one_over_end_minus_start = 1.0f / (morphEnd - morphStart);
    _morphConstants.set(morphEnd * one_over_end_minus_start, one_over_end_minus_start);

    // Make a tilekey to use for testing whether to subdivide.
    if (_key.getTileY() <= th / 2)
        _subdivideTestKey = _key.createChildKey(0);
    else
        _subdivideTestKey = _key.createChildKey(3);
}

TileNode::~TileNode()
{
    //nop
}

void
TileNode::setDoNotExpire(bool value)
{
    _doNotExpire = value;
}

void
TileNode::createGeometry(Cancelable* progress)
{
    osg::ref_ptr<const Map> map(_context->getMap());
    if (!map.valid())
        return;

    _empty = false;

    unsigned tileSize = options().tileSize().get();

    // Get a shared geometry from the pool that corresponds to this tile key:
    osg::ref_ptr<SharedGeometry> geom;

    _context->getGeometryPool()->getPooledGeometry(
        _key,
        tileSize,
        map.get(),
        options(),
        geom,
        progress);

    if (progress && progress->isCanceled())
        return;

    if (geom.valid())
    {
        // Create the drawable for the terrain surface:
        TileDrawable* surfaceDrawable = new TileDrawable(
            _key,
            geom.get(),
            tileSize);

        // Give the tile Drawable access to the render model so it can properly
        // calculate its bounding box and sphere.
        // TODO:  This is really only used if you have a shader that modifies the bounding box.  Don't comment out.
        surfaceDrawable->setModifyBBoxCallback(_context->getModifyBBoxCallback());

        osg::ref_ptr<const osg::Image> elevationRaster = getElevationRaster();
        osg::Matrixf elevationMatrix = getElevationMatrix();

        // Create the node to house the tile drawable:
        _surface = new SurfaceNode(_key, surfaceDrawable);

        if (elevationRaster.valid())
        {
            _surface->setElevationRaster(elevationRaster.get(), elevationMatrix);
        }
    }
    else
    {
        _empty = true;
    }

    dirtyBound();
}


struct CollectTriangles
{
    CollectTriangles()
    {
        verts = new osg::Vec3Array();
    }
#if OSG_VERSION_LESS_THAN(3,5,6)
    inline void operator () (const osg::Vec3& v1, const osg::Vec3& v2, const osg::Vec3& v3, bool treatVertexDataAsTemporary)
#else
    inline void operator () (const osg::Vec3& v1, const osg::Vec3& v2, const osg::Vec3& v3)
#endif
    {
        verts->push_back(v1);
        verts->push_back(v2);
        verts->push_back(v3);
    }

    osg::ref_ptr< osg::Vec3Array > verts;
};

struct CollectTrianglesVisitor : public osg::NodeVisitor
{
    CollectTrianglesVisitor() :
        //osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ACTIVE_CHILDREN)
        osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN)
    {
        _vertices.reserve(1000000);
    }

    void apply(osg::Transform& transform)
    {
        osg::Matrix matrix;
        if (!_matrixStack.empty()) matrix = _matrixStack.back();
        transform.computeLocalToWorldMatrix(matrix, this);
        pushMatrix(matrix);
        traverse(transform);
        popMatrix();
    }

    void apply(osg::Drawable& drawable) override
    {
        osg::TriangleFunctor<CollectTriangles> triangleCollector;
        drawable.accept(triangleCollector);
        for (unsigned int j = 0; j < triangleCollector.verts->size(); j++)
        {
            static osg::Matrix identity;
            osg::Matrix& matrix = _matrixStack.empty() ? identity : _matrixStack.back();
            osg::Vec3d v = (*triangleCollector.verts)[j];
            _vertices.emplace_back(v * matrix);
        }
    }

    float getDistanceToEyePoint(const osg::Vec3& pos, bool /*withLODScale*/) const
    {
        // Use highest level of detail
        return 0.0;
    }

    osg::Node* buildNode()
    {
        osg::Geometry* geom = new osg::Geometry;
        osg::Vec3Array* verts = new osg::Vec3Array;
        geom->setVertexArray(verts);

        bool first = true;
        osg::Vec3d anchor;

        for (unsigned int i = 0; i < _vertices.size(); i++)
        {
            if (first)
            {
                anchor = _vertices[i];
                first = false;
            }
            verts->push_back(_vertices[i] - anchor);
        }
        geom->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, verts->size()));

        osg::MatrixTransform* mt = new osg::MatrixTransform;
        mt->setReferenceFrame(osg::MatrixTransform::ABSOLUTE_RF);
        mt->setMatrix(osg::Matrixd::translate(anchor));
        mt->addChild(geom);

        return mt;
    }

    inline void pushMatrix(osg::Matrix& matrix) { _matrixStack.push_back(matrix); }

    inline void popMatrix() { _matrixStack.pop_back(); }

    typedef std::vector<osg::Matrix> MatrixStack;
    std::vector<osg::Vec3d>  _vertices;
    MatrixStack _matrixStack;
};


void
TileNode::initializeData()
{
    // Initialize the data model by copying the parent's rendering data
    // and scale/biasing the matrices.

    TileNode* parent = getParentTile();
    if (parent)
    {
        unsigned quadrant = getKey().getQuadrant();

        const RenderBindings& bindings = _context->getRenderBindings();

        for (unsigned p = 0; p < parent->_renderModel._passes.size(); ++p)
        {
            const RenderingPass& parentPass = parent->_renderModel._passes[p];

            // If the key is now out of the layer's valid min/max range, skip this pass.
            if (!passInLegalRange(parentPass))
                continue;

            // Copy the parent pass:
            _renderModel._passes.push_back(parentPass);
            RenderingPass& myPass = _renderModel._passes.back();
            myPass.setParent(&parentPass);

            // Scale/bias each matrix for this key quadrant.
            Samplers& samplers = myPass.samplers();
            for (unsigned s = 0; s < samplers.size(); ++s)
            {
                samplers[s]._matrix.preMult(scaleBias[quadrant]);
            }

            // Are we using image blending? If so, initialize the color_parent
            // to the color texture.
            if (bindings[SamplerBinding::COLOR_PARENT].isActive())
            {
                samplers[SamplerBinding::COLOR_PARENT] = samplers[SamplerBinding::COLOR];
            }
        }

        // Copy the parent's shared samplers and scale+bias each matrix to the new quadrant:
        _renderModel._sharedSamplers = parent->_renderModel._sharedSamplers;

        for (unsigned s = 0; s<_renderModel._sharedSamplers.size(); ++s)
        {
            Sampler& sampler = _renderModel._sharedSamplers[s];
            sampler._matrix.preMult(scaleBias[quadrant]);
        }

        // Use the elevation sampler to initialize the elevation raster
        // (used for primitive functors, intersection, etc.)
        if (bindings[SamplerBinding::ELEVATION].isActive())
        {
            updateElevationRaster();
        }
    }

#if 0
    unsigned int colliderLevel = 16;
    if (_key.getLevelOfDetail() == colliderLevel)
    {
        TerrainTileModelFactory factory(_context->_options);


        // Only load elevation
        CreateTileManifest manifest;
        ElevationLayerVector elevation;
        _context->getMap()->getLayers(elevation);
        for (auto &l : elevation)
        {
            manifest.insert(l.get());
        }

        osg::ref_ptr<TerrainTileModel> model =
            factory.createStandaloneTileModel(_context->_map.get(), _key, manifest, nullptr, nullptr);

        unsigned int refLOD = _context->_selectionInfo.getNumLODs() - 1;
        osg::ref_ptr<osg::Node> node =
            _context->_terrainEngine->createStandaloneTile(model.get(), TerrainEngineNode::CREATE_TILE_INCLUDE_ALL, refLOD, _key);

        if (node.valid())
        {
            CollectTrianglesVisitor v;
            node->accept(v);
            std::cout << "Collider has " << v._vertices.size() / 3 << " triangles" << std::endl;
            osg::ref_ptr< osg::Node > collider = v.buildNode();
            // Build kdtrees
            osg::ref_ptr< osg::KdTreeBuilder > kdTreeBuilder = new osg::KdTreeBuilder();
            collider->accept(*kdTreeBuilder.get());
            collider->setName("COLLIDER");
            getOrCreateUserDataContainer()->addUserObject(collider);
        }
    }
#endif

    // register me.
    _context->liveTiles()->add( this );

    // tell the world.
    OE_DEBUG << LC << "notify (create) key " << getKey().str() << std::endl;
    _context->getEngine()->getTerrain()->notifyTileUpdate(getKey(), this);
}

osg::BoundingSphere
TileNode::computeBound() const
{
    osg::BoundingSphere bs;
    if (_surface.valid())
    {
        bs = _surface->getBound();
        const osg::BoundingBox& bbox = _surface->getAlignedBoundingBox();
        _tileKeyValue.a() = osg::maximum( (bbox.xMax()-bbox.xMin()), (bbox.yMax()-bbox.yMin()) );
    }
    return bs;
}

bool
TileNode::isDormant() const
{
    const unsigned minMinExpiryFrames = 3u;
    unsigned frame = _context->getClock()->getFrame();
    double now = _context->getClock()->getTime();

    bool dormant =
        frame - _lastTraversalFrame > osg::maximum(options().minExpiryFrames().get(), minMinExpiryFrames) &&
        now - _lastTraversalTime > options().minExpiryTime().get();

    return dormant;
}

bool
TileNode::areSiblingsDormant() const
{
    const TileNode* parent = getParentTile();
    return parent ? parent->areSubTilesDormant() : true;
}

void
TileNode::setElevationRaster(const osg::Image* image, const osg::Matrixf& matrix)
{
    if (image != getElevationRaster() || matrix != getElevationMatrix())
    {
        if ( _surface.valid() )
            _surface->setElevationRaster( image, matrix );
    }
}

void
TileNode::updateElevationRaster()
{
    const Sampler& elev = _renderModel._sharedSamplers[SamplerBinding::ELEVATION];
    if (elev._texture.valid())
        setElevationRaster(elev._texture->getImage(0), elev._matrix);
    else
        setElevationRaster(NULL, osg::Matrixf::identity());
}

const osg::Image*
TileNode::getElevationRaster() const
{
    return _surface.valid() ? _surface->getElevationRaster() : 0L;
}

const osg::Matrixf&
TileNode::getElevationMatrix() const
{
    static osg::Matrixf s_identity;
    return _surface.valid() ? _surface->getElevationMatrix() : s_identity;
}

void
TileNode::refreshAllLayers()
{
    refreshLayers(CreateTileManifest());

#if 0
    // Only load imagery....
    CreateTileManifest manifest;
    ImageLayerVector images;
    _context->getMap()->getLayers(images);
    for (auto &l : images)
    {
        manifest.insert(l.get());
    }
    refreshLayers(manifest);
#endif
}

void
TileNode::refreshLayers(const CreateTileManifest& manifest)
{
    LoadTileDataOperationPtr r =
        std::make_shared<LoadTileDataOperation>(manifest, this, _context.get());

    _loadQueue.lock();
    _loadQueue.push(r);
    _loadsInQueue = _loadQueue.size();
    if (_loadsInQueue > 0)
        _nextLoadManifestPtr = &_loadQueue.front()->_manifest;
    else
        _nextLoadManifestPtr = nullptr;
    _loadQueue.unlock();
}

void
TileNode::releaseGLObjects(osg::State* state) const
{
    osg::Group::releaseGLObjects(state);

    if ( _surface.valid() )
        _surface->releaseGLObjects(state);

    _renderModel.releaseGLObjects(state);
}

void
TileNode::resizeGLObjectBuffers(unsigned maxSize)
{
    osg::Group::resizeGLObjectBuffers(maxSize);

    if ( _surface.valid() )
        _surface->resizeGLObjectBuffers(maxSize);

    _renderModel.resizeGLObjectBuffers(maxSize);
}

bool
TileNode::shouldSubDivide(TerrainCuller* culler, const SelectionInfo& selectionInfo)
{
    unsigned currLOD = _key.getLOD();

    EngineContext* context = culler->getEngineContext();

    if (currLOD < selectionInfo.getNumLODs() && currLOD != selectionInfo.getNumLODs()-1)
    {
        // In PSOS mode, subdivide when the on-screen size of a tile exceeds the maximum
        // allowable on-screen tile size in pixels.
        if (options().rangeMode() == osg::LOD::PIXEL_SIZE_ON_SCREEN)
        {
            float tileSizeInPixels = -1.0;

            if (context->getEngine()->getComputeRangeCallback())
            {
                tileSizeInPixels = (*context->getEngine()->getComputeRangeCallback())(this, *culler->_cv);
            }

            if (tileSizeInPixels <= 0.0)
            {
                tileSizeInPixels = _surface->getPixelSizeOnScreen(culler);
            }

            return (tileSizeInPixels > options().tilePixelSize().get());
        }

        // In DISTANCE-TO-EYE mode, use the visibility ranges precomputed in the SelectionInfo.
        else
        {
            float range = context->getSelectionInfo().getRange(_subdivideTestKey);
#if 1
            // slightly slower than the alternate block below, but supports a user overriding
            // CullVisitor::getDistanceToViewPoint -gw
            return _surface->anyChildBoxWithinRange(range, *culler);
#else
            return _surface->anyChildBoxIntersectsSphere(
                culler->getViewPointLocal(),
                range*range / culler->getLODScale());
#endif
        }
    }
    return false;
}

bool
TileNode::cull_spy(TerrainCuller* culler)
{
    bool visible = false;

    EngineContext* context = culler->getEngineContext();

    // Shows all culled tiles. All this does is traverse the terrain
    // and add any tile that's been "legitimately" culled (i.e. culled
    // by a non-spy traversal) in the last 2 frames. We use this
    // trick to spy on another camera.
    unsigned frame = context->getClock()->getFrame();

    if ( frame - _surface->getLastFramePassedCull() < 2u)
    {
        _surface->accept( *culler );
    }

    else if ( _childrenReady )
    {
        for(int i=0; i<4; ++i)
        {
            TileNode* child = getSubTile(i);
            if (child)
                child->accept( *culler );
        }
    }

    return visible;
}

#define LOAD_NORMALLY

bool
TileNode::cull(TerrainCuller* culler)
{
    EngineContext* context = culler->getEngineContext();

    // Horizon check the surface first:
    if (!_surface->isVisibleFrom(culler->getViewPointLocal()))
    {
        return false;
    }

    // determine whether we can and should subdivide to a higher resolution:
    bool childrenInRange = shouldSubDivide(culler, context->getSelectionInfo());

    // whether it is OK to create child TileNodes is necessary.
    bool canCreateChildren = childrenInRange;

    // whether it is OK to load data if necessary.
    bool canLoadData = true;

    const TerrainOptions& opt = _context->options();
    canLoadData =
        _doNotExpire ||
        _key.getLOD() == opt.firstLOD().get() ||
        _key.getLOD() >= opt.minLOD().get();

    // whether to accept the current surface node and not the children.
    bool canAcceptSurface = false;

    // If this is an inherit-viewpoint camera, we don't need it to invoke subdivision
    // because we want only the tiles loaded by the true viewpoint.
    const osg::Camera* cam = culler->getCamera();
    if (cam && cam->getReferenceFrame() == osg::Camera::ABSOLUTE_RF_INHERIT_VIEWPOINT)
    {
        canCreateChildren = false;
        canLoadData = false;
    }

    else
    {
        // TODO:  This makes sure the parent loads it's data before we can load ours.
        // Don't load data OR geometry in progressive mode until the parent is up to date
        if (options().progressive() == true)
        {
            TileNode* parent = getParentTile();
            if (parent && parent->dirty() && parent->nextLoadIsProgressive())
            {
                canLoadData = false;

                // comment this out if you want to load the geometry, but not the data --
                // this will allow the terrain to always show the higest tessellation level
                // even as the data is still loading ..
                // TODO:  this might be a good thing to comment out :)
                //canCreateChildren = false;
            }
        }
    }

    if (childrenInRange)
    {
        // We are in range of the child nodes. Either draw them or load them.

        // If the children don't exist, create them and inherit the parent's data.
        if ( !_childrenReady && canCreateChildren )
        {
            _mutex.lock();

            if ( !_childrenReady ) // double check inside mutex
            {
#ifdef LOAD_NORMALLY
                // TODO:  Comment this out to not subdivide in the cull traversal
                _childrenReady = createChildren( context );
                // This means that you cannot start loading data immediately; must wait a frame.
                canLoadData = false;
#endif
            }

            _mutex.unlock();
        }

        // If all are ready, traverse them now.
        if ( _childrenReady )
        {
            for(int i=0; i<4; ++i)
            {
                TileNode* child = getSubTile(i);
                if (child)
                    child->accept(*culler);
            }
        }

        // If we don't traverse the children, traverse this node's payload.
        else
        {
            canAcceptSurface = true;
        }
    }

    // If children are outside camera range, draw the payload and expire the children.
    else
    {
        canAcceptSurface = true;
    }

    // accept this surface if necessary.
    if ( canAcceptSurface )
    {
        _surface->accept( *culler );
    }

    // If this tile is marked dirty, try loading data.
    if ( dirty() && canLoadData )
    {
#ifdef LOAD_NORMALLY
        // Don't load in cull to see if we are actually loading stuff correctly ourselves.
        load( culler );
#endif
    }

    return true;
}

bool
TileNode::accept_cull(TerrainCuller* culler)
{
    bool visible = false;

    if (culler)
    {
        if ( !culler->isCulled(*this) )
        {
            visible = cull( culler );
        }
    }

    return visible;
}

bool
TileNode::accept_cull_spy(TerrainCuller* culler)
{
    bool visible = false;

    if (culler)
    {
        visible = cull_spy( culler );
    }

    return visible;
}

void
TileNode::traverse(osg::NodeVisitor& nv)
{
    // Cull only:
    if ( nv.getVisitorType() == nv.CULL_VISITOR )
    {
        TerrainCuller* culler = dynamic_cast<TerrainCuller*>(&nv);

        // update the timestamp so this tile doesn't become dormant.
        _lastTraversalFrame.exchange(_context->getClock()->getFrame());
        _lastTraversalTime = _context->getClock()->getTime();

        _context->liveTiles()->update(this, nv);

        if (_empty)
        {
            // even if the tile's empty, we need to process its load queue
            if (dirty())
            {
                load(culler);
            }
        }
        else
        {
            if (culler->_isSpy)
            {
                accept_cull_spy( culler );
            }
            else
            {
                accept_cull( culler );
            }
        }
    }

    // Everything else: update, GL compile, intersection, compute bound, etc.
    else
    {
        // Check for image updates.
        if (nv.getVisitorType() == nv.UPDATE_VISITOR && _imageUpdatesActive)
        {
            unsigned numUpdatedTotal = 0u;
            unsigned numFuturesResolved = 0u;

            for (unsigned p = 0; p < _renderModel._passes.size(); ++p)
            {
                RenderingPass& pass = _renderModel._passes[p];
                Samplers& samplers = pass.samplers();
                for (unsigned s = 0; s < samplers.size(); ++s)
                {
                    Sampler& sampler = samplers[s];

                    if (sampler.ownsTexture())
                    {
                        for(unsigned i = 0; i < sampler._texture->getNumImages(); ++i)
                        {
                            osg::Image* image = sampler._texture->getImage(i);
                            if (image && image->requiresUpdateCall())
                            {
                                image->update(&nv);
                                numUpdatedTotal++;
                            }
                        }
                    }

                    // handle "future" textures. This is a texture that was installed
                    // by an "async" image layer that is working in the background
                    // to load. Once it is available we can merge it into the real texture
                    // slot for rendering.
                    if (sampler._futureTexture.valid())
                    {
                        unsigned levelsDoneUpdating = sampler._futureTexture->getNumImages();
                        unsigned numUpdated = 0;

                        for (unsigned i = 0; i < sampler._futureTexture->getNumImages(); ++i)
                        {
                            osg::Image* image = sampler._futureTexture->getImage(i);
                            if (image)
                            {
                                if (image->requiresUpdateCall())
                                {
                                    //OE_INFO << _key.str() << " image->update..." << std::endl;
                                    image->update(&nv);
                                    numUpdated++;
                                    numUpdatedTotal++;
                                }

                                // an image with a valid size indicates the job is complete
                                if (image->s() > 0)
                                {
                                    --levelsDoneUpdating;
                                }
                            }
                        }

                        // when all images are complete, update the texture and discard the future object.
                        if (levelsDoneUpdating == 0)
                        {
                            sampler._texture = sampler._futureTexture;
                            sampler._matrix.makeIdentity();
                            sampler._futureTexture = nullptr;
                            ++numFuturesResolved;
                        }

                        else if (numUpdated == 0)
                        {
                            // can happen if the asynchronous request fails.
                            sampler._futureTexture = nullptr;
                        }
                    }
                }
            }

            // if no updates were detected, don't check next time.
            if (numUpdatedTotal == 0)
            {
                ADJUST_UPDATE_TRAV_COUNT(this, -1);
                _imageUpdatesActive = false;
            }

            // if we resolve any future-textures, inform the children
            // that they need to update their inherited samplers.
            if (numFuturesResolved > 0)
            {
                for (int i = 0; i < 4; ++i)
                {
                    if ((int)getNumChildren() > i)
                    {
                        TileNode* child = getSubTile(i);
                        if (child)
                            child->refreshInheritedData(this, _context->getRenderBindings());
                    }
                }
            }
        }

        // If there are child nodes, traverse them:
        int numChildren = getNumChildren();
        if (numChildren > 0)
        {
            for (int i = 0; i < numChildren; ++i)
            {
                _children[i]->accept(nv);
            }
        }

        // Otherwise traverse the surface.
        else if (_surface.valid())
        {
            _surface->accept(nv);
        }
    }
}

bool
TileNode::createChildren(EngineContext* context)
{
    if (_createChildAsync)
    {
        if (_createChildResults.empty())
        {
            TileKey parentkey(_key);

            for (unsigned quadrant = 0; quadrant < 4; ++quadrant)
            {
                TileKey childkey = getKey().createChildKey(quadrant);

                auto op = [context, parentkey, childkey](Cancelable* state)
                {
                    osg::ref_ptr<TileNode> tile = context->liveTiles()->get(parentkey);
                    if (tile.valid() && !state->isCanceled())
                        return tile->createChild(childkey, context, state);
                    else
                        return (TileNode*)nullptr;
                };

                Job job;
                job.setArena(ARENA_CREATE_CHILD);
                job.setName(childkey.str());

                _createChildResults.emplace_back(
                    job.dispatch<CreateChildResult>(op)
                );
            }
        }

        else
        {
            int numChildrenReady = 0;

            for (int i = 0; i < 4; ++i)
            {
                if (_createChildResults[i].isAvailable())
                    ++numChildrenReady;
            }

            if (numChildrenReady == 4)
            {
                for (int i = 0; i < 4; ++i)
                {
                    osg::ref_ptr<TileNode> child = _createChildResults[i].get();
                    addChild(child);

                    // sets up inheritence
                    child->initializeData();

                    // TODO:
                    // actually loads data
                    // When you try to load an lod 19 tile and call refreshInherited data it won't actually load anything b/c the data doesn't exist there, only at 10.
                    // So it's dependent on the lod 10 being loaded.
                    // You could check to see what the max elevation data is at a certain level like 10-19 and only load.
                    // We shouldn't do this automatically here or have an option or something......
                    child->refreshAllLayers();
                }

                _createChildResults.clear();
            }
        }
    }

    else
    {
        for (unsigned quadrant = 0; quadrant < 4; ++quadrant)
        {
            TileKey childkey = getKey().createChildKey(quadrant);
            osg::ref_ptr<TileNode> child = createChild(childkey, context, nullptr);
            addChild(child);
            child->initializeData();
            child->refreshAllLayers();
        }
    }

    return _createChildResults.empty();
}

TileNode*
TileNode::createChild(const TileKey& childkey, EngineContext* context, Cancelable* progress)
{
    OE_PROFILING_ZONE;

    osg::ref_ptr<TileNode> node = new TileNode(
        childkey,
        this, // parent TileNode
        context,
        progress);

    return
        progress && progress->isCanceled() ? nullptr
        : node.release();
}

void
TileNode::merge(
    const TerrainTileModel* model,
    const CreateTileManifest& manifest)
{
    bool newElevationData = false;
    const RenderBindings& bindings = _context->getRenderBindings();
    RenderingPasses& myPasses = _renderModel._passes;
    vector_set<UID> uidsLoaded;

    // if terrain constraints are in play, regenerate the tile's geometry.
    // this could be kinda slow, but meh, if you are adding and removing
    // constraints, frame drops are not a big concern
    if (manifest.includesConstraints())
    {
        // todo: progress callback here? I believe progress gets
        // checked before merge() anyway.
        createGeometry(nullptr);
    }

    // First deal with the rendering passes (for color data):
    const SamplerBinding& color = bindings[SamplerBinding::COLOR];
    if (color.isActive())
    {
        // loop over all the layers included in the new data model and
        // add them to our render model (or update them if they already exist)
        for(const auto& colorLayerModel : model->colorLayers())
        {
            if (!colorLayerModel.valid())
                continue;

            const Layer* layer = colorLayerModel->getLayer();
            if (!layer)
                continue;

            // Look up the parent pass in case we need it
            RenderingPass* pass =
                _renderModel.getPass(layer->getUID());

            const RenderingPass* parentPass =
                pass ? pass->parent() :
                getParentTile() ? getParentTile()->_renderModel.getPass(layer->getUID()) :
                nullptr;

            // ImageLayer?
            TerrainTileImageLayerModel* imageLayerModel = dynamic_cast<TerrainTileImageLayerModel*>(colorLayerModel.get());
            if (imageLayerModel && imageLayerModel->getTexture())
            {
                bool isNewPass = (pass == nullptr);
                if (isNewPass)
                {
                    // Pass didn't exist here, so add it now.
                    pass = &_renderModel.addPass(parentPass);
                    pass->setLayer(layer);
                }

                pass->setSampler(SamplerBinding::COLOR, imageLayerModel->getTexture(), *imageLayerModel->getMatrix(), imageLayerModel->getRevision());

                // If this is a new rendering pass, just copy the color into the color-parent.
                if (isNewPass && bindings[SamplerBinding::COLOR_PARENT].isActive())
                {
                    pass->sampler(SamplerBinding::COLOR_PARENT) = pass->sampler(SamplerBinding::COLOR);
                }

                // check to see if this data requires an image update traversal.
                if (_imageUpdatesActive == false)
                {
                    osg::Texture* texture = imageLayerModel->getTexture();

                    for (unsigned i = 0; i < texture->getNumImages(); ++i)
                    {
                        const osg::Image* image = texture->getImage(i);
                        if (image && image->requiresUpdateCall())
                        {
                            ADJUST_UPDATE_TRAV_COUNT(this, +1);
                            _imageUpdatesActive = true;
                            break;
                        }
                    }
                }

                if (imageLayerModel->getImageLayer()->getAsyncLoading())
                {
                    if (pass->parent())
                    {
                        pass->inheritFrom(*pass->parent(), scaleBias[_key.getQuadrant()]);

                        if (bindings[SamplerBinding::COLOR_PARENT].isActive())
                        {
                            Sampler& colorParent = pass->sampler(SamplerBinding::COLOR_PARENT);
                            colorParent._texture = pass->parent()->sampler(SamplerBinding::COLOR)._texture;
                            colorParent._matrix = pass->parent()->sampler(SamplerBinding::COLOR)._matrix;
                            colorParent._matrix.preMult(scaleBias[_key.getQuadrant()]);
                        }
                    }
                    else
                    {
                        // note: this can happen with an async layer load
                        OE_DEBUG << "no parent pass in my pass. key=" << model->getKey().str() << std::endl;
                    }

                    pass->sampler(SamplerBinding::COLOR)._futureTexture = imageLayerModel->getTexture();
                }

                uidsLoaded.insert(pass->sourceUID());
            }

            else // non-image color layer (like splatting, e.g.)
            {
                if (!pass)
                {
                    pass = &_renderModel.addPass(parentPass);
                    pass->setLayer(colorLayerModel->getLayer());
                }

                uidsLoaded.insert(pass->sourceUID());
            }
        }

        // Next loop over all the passes that we OWN, we asked for, but we didn't get.
        // That means they no longer exist at this LOD, and we need to convert them
        // into inherited samplers (or delete them entirely)
        for(int p=0; p<myPasses.size(); ++p)
        {
            RenderingPass& myPass = myPasses[p];

            if (myPass.ownsTexture() &&
                manifest.includes(myPass.layer()) &&
                !uidsLoaded.contains(myPass.sourceUID()))
            {
                OE_DEBUG << LC << "Releasing orphaned layer " << myPass.layer()->getName() << std::endl;

                // release the GL objects associated with this pass.
                // taking this out...can cause "flashing" issues
                //myPass.releaseGLObjects(NULL);

                bool deletePass = true;

                TileNode* parent = getParentTile();
                if (parent)
                {
                    const RenderingPass* parentPass = parent->_renderModel.getPass(myPass.sourceUID());
                    if (parentPass)
                    {
                        myPass.inheritFrom(*parentPass, scaleBias[_key.getQuadrant()]);
                        deletePass = false;
                    }
                }

                if (deletePass)
                {
                    myPasses.erase(myPasses.begin() + p);
                    --p;
                }
            }
        }
    }

    // Elevation data:
    const SamplerBinding& elevation = bindings[SamplerBinding::ELEVATION];
    if (elevation.isActive())
    {
        if (model->elevationModel().valid() && model->elevationModel()->getTexture())
        {
            osg::Texture* tex = model->elevationModel()->getTexture();
            int revision = model->elevationModel()->getRevision();

            _renderModel.setSharedSampler(SamplerBinding::ELEVATION, tex, revision);

            //setElevationRaster(tex->getImage(0), osg::Matrixf::identity());
            updateElevationRaster();

            newElevationData = true;
        }

        else if (
            manifest.includesElevation() &&
            _renderModel._sharedSamplers[SamplerBinding::ELEVATION].ownsTexture())
        {
            // We OWN elevation data, requested new data, and didn't get any.
            // That means it disappeared and we need to delete what we have.
            inheritSharedSampler(SamplerBinding::ELEVATION);

            updateElevationRaster();

            newElevationData = true;
        }
    }

    // Normals:
    const SamplerBinding& normals = bindings[SamplerBinding::NORMAL];
    if (normals.isActive())
    {
        if (model->elevationModel().valid() && model->elevationModel()->getTexture())
        {
            ElevationTexture* etex = static_cast<ElevationTexture*>(model->elevationModel()->getTexture());
            if (etex->getNormalMapTexture())
            {
                osg::Texture* tex = etex->getNormalMapTexture();
                int revision = model->elevationModel()->getRevision();

                if (_context->options().normalizeEdges() == true)
                {
                    // keep the normal map around because we might update it later
                    tex->setUnRefImageDataAfterApply(false);
                }

                _renderModel.setSharedSampler(SamplerBinding::NORMAL, tex, revision);
                updateNormalMap();
            }
        }

        //if (model->normalModel().valid() && model->normalModel()->getTexture())
        //{
        //    osg::Texture* tex = model->normalModel()->getTexture();
        //    int revision = model->normalModel()->getRevision();

        //    if (_context->options().normalizeEdges() == true)
        //    {
        //        // keep the normal map around because we might update it later
        //        tex->setUnRefImageDataAfterApply(false);
        //    }

        //    _renderModel.setSharedSampler(SamplerBinding::NORMAL, tex, revision);
        //    updateNormalMap();
        //}

        // If we OWN normal data, requested new data, and didn't get any,
        // that means it disappeared and we need to delete what we have:
        else if (
            manifest.includesElevation() && // not a typo, check for elevation
            _renderModel._sharedSamplers[SamplerBinding::NORMAL].ownsTexture())
        {
            inheritSharedSampler(SamplerBinding::NORMAL);
            updateNormalMap();
        }
    }

    // Land Cover:
    const SamplerBinding& landCover = bindings[SamplerBinding::LANDCOVER];
    if (landCover.isActive())
    {
        if (model->landCoverModel().valid() && model->landCoverModel()->getTexture())
        {
            osg::Texture* tex = model->landCoverModel()->getTexture();
            int revision = model->landCoverModel()->getRevision();

            _renderModel.setSharedSampler(SamplerBinding::LANDCOVER, tex, revision);
        }

        else if (
            manifest.includesLandCover() &&
            _renderModel._sharedSamplers[SamplerBinding::LANDCOVER].ownsTexture())
        {
            // We OWN landcover data, requested new data, and didn't get any.
            // That means it disappeared and we need to delete what we have.
            inheritSharedSampler(SamplerBinding::LANDCOVER);
        }
    }

    // Other Shared Layers:
    uidsLoaded.clear();
    for (unsigned i = 0; i < model->sharedLayers().size(); ++i)
    {
        TerrainTileImageLayerModel* layerModel = model->sharedLayers()[i].get();
        if (layerModel->getTexture())
        {
            // locate the shared binding corresponding to this layer:
            UID uid = layerModel->getImageLayer()->getUID();
            unsigned bindingIndex = INT_MAX;
            for(unsigned i=SamplerBinding::SHARED; i<bindings.size() && bindingIndex==INT_MAX; ++i)
            {
                if (bindings[i].isActive() && bindings[i].sourceUID().isSetTo(uid))
                {
                    bindingIndex = i;
                }
            }

            if (bindingIndex < INT_MAX)
            {
                osg::Texture* tex = layerModel->getTexture();
                int revision = layerModel->getRevision();
                _renderModel.setSharedSampler(bindingIndex, tex, revision);
                uidsLoaded.insert(uid);
            }
        }
    }

    // Look for shared layers we need to remove because we own them,
    // requested them, and didn't get updates for them:
    for(unsigned i=SamplerBinding::SHARED; i<bindings.size(); ++i)
    {
        if (bindings[i].isActive() &&
            manifest.includes(bindings[i].sourceUID().get()) &&
            !uidsLoaded.contains(bindings[i].sourceUID().get()))
        {
            inheritSharedSampler(i);
        }
    }

    // Patch Layers - NOP for now
#if 0
    for (unsigned i = 0; i < model->patchLayers().size(); ++i)
    {
        TerrainTilePatchLayerModel* layerModel = model->patchLayers()[i].get();
    }
#endif

    // Propagate changes we made down to this tile's children.
    if (_childrenReady)
    {
        for (int i = 0; i < 4; ++i)
        {
            TileNode* child = getSubTile(i);
            if (child)
            {
                child->refreshInheritedData(this, bindings);
            }
        }
    }

    if (newElevationData)
    {
        _context->getEngine()->getTerrain()->notifyTileUpdate(getKey(), this);
    }

    // Bump the data revision for the tile.
    ++_revision;

    _merged = true;
}

void TileNode::inheritSharedSampler(int binding)
{
    TileNode* parent = getParentTile();
    if (parent)
    {
        TileRenderModel& parentModel = parent->_renderModel;
        Sampler& mySampler = _renderModel._sharedSamplers[binding];
        mySampler = parentModel._sharedSamplers[binding];
        if (mySampler._texture.valid())
            mySampler._matrix.preMult(scaleBias[_key.getQuadrant()]);
    }
    else
    {
        _renderModel.clearSharedSampler(binding);
    }

    // Bump the data revision for the tile.
    ++_revision;
}

//void TileNode::loadChildren()
//{
//    _mutex.lock();
//
//    if ( !_childrenReady )
//    {
//        // Create the children
//        createChildren( _context.get() );
//        _childrenReady = true;
//        int numChildren = getNumChildren();
//        if ( numChildren > 0 )
//        {
//            for(int i=0; i<numChildren; ++i)
//            {
//                TileNode* child = getSubTile(i);
//                if (child)
//                {
//                    // Load the children's data.
//                    child->loadSync();
//                }
//            }
//        }
//    }
//    _mutex.unlock();
//}


// LoadableNode
void TileNode::load()
{
    processLoadQueue(nullptr);
}

void TileNode::unload()
{
}

RefinePolicy TileNode::getRefinePolicy() const
{
    return REFINE_REPLACE;
}

bool TileNode::isLoaded() const
{
    // What should isLoaded be driven off of?
    // TODO:  Maybe when we get the "highest res data" we can make a super high res kdtree where it can stop.
    // We'd have to use a kdtree where we set our own verts on it that are higher res than the 17x17 one.
    //return isHighestResolution() || _childrenReady;
    return _merged;
}

bool TileNode::isHighestResolution() const
{
    const SelectionInfo& si = _context->getSelectionInfo();
    return _key.getLOD() == si.getNumLODs() - 1;
}

bool TileNode::getAutoUnload() const
{
    return !getDoNotExpire();
}

void TileNode::setAutoUnload(bool value)
{
    setDoNotExpire(!value);
}

bool TileNode::canSubdivide() const
{
    return !isHighestResolution() && !_childrenReady;
}

void TileNode::subdivide()
{
    ScopedMutexLock lock(_mutex);

    const SelectionInfo& si = _context->getSelectionInfo();
    if (_key.getLOD() != si.getNumLODs() - 1)
    {
        if (!_childrenReady) // double check inside mutex
        {
            _childrenReady = createChildren(_context.get());
        }
    }
}

void
TileNode::refreshSharedSamplers(const RenderBindings& bindings)
{
    for (unsigned i = 0; i < _renderModel._sharedSamplers.size(); ++i)
    {
        if (bindings[i].isActive() == false)
        {
            _renderModel.clearSharedSampler(i);
        }
    }
}

void
TileNode::refreshInheritedData(TileNode* parent, const RenderBindings& bindings)
{
    // Run through this tile's rendering data and re-inherit textures and matrixes
    // from the parent. When a TileNode gets new data (via a call to merge), any
    // children of that tile that are inheriting textures or matrixes need to
    // refresh to inherit that new data. In turn, those tile's children then need
    // to update as well. This method does that.

    // which quadrant is this tile in?
    unsigned quadrant = getKey().getQuadrant();

    // Count the number of changes we make so we can stop early if it's OK.
    unsigned changes = 0;

    RenderingPasses& parentPasses = parent->_renderModel._passes;
    RenderingPasses& myPasses = _renderModel._passes;

    // Delete any inherited pass whose parent pass no longer exists:
    for(int p=0; p<myPasses.size(); ++p)
    {
        RenderingPass& myPass = myPasses[p];
        if (myPass.inheritsTexture())
        {
            RenderingPass* myParentPass = parent->_renderModel.getPass(myPass.sourceUID());
            if (myParentPass == nullptr)
            {
                //OE_WARN << _key.str() << " removing orphaned pass " << myPass.sourceUID() << std::endl;
                myPasses.erase(myPasses.begin()+p);
                --p;
                ++changes;
            }
        }
    }

    // Look for passes in the parent that need to be inherited by this node.
    for (unsigned p=0; p<parentPasses.size(); ++p)
    {
        const RenderingPass& parentPass = parentPasses[p];

        // the corresponsing pass in this node:
        RenderingPass* myPass = _renderModel.getPass(parentPass.sourceUID());

        // Inherit the samplers for this pass.
        if (myPass)
        {
            // Handle the main color:
            if (bindings[SamplerBinding::COLOR].isActive())
            {
                Sampler& mySampler = myPass->sampler(SamplerBinding::COLOR);
                if (mySampler.inheritsTexture())
                {
                    mySampler.inheritFrom(parentPass.sampler(SamplerBinding::COLOR), scaleBias[quadrant]);
                    ++changes;
                }
            }

            // Handle the parent color. This is special case -- the parent
            // sampler is always set to the parent's color sampler with a
            // one-time scale/bias.
            if (bindings[SamplerBinding::COLOR_PARENT].isActive())
            {
                Sampler& mySampler = myPass->sampler(SamplerBinding::COLOR_PARENT);
                const Sampler& parentColorSampler = parentPass.sampler(SamplerBinding::COLOR);
                osg::Matrixf newMatrix = parentColorSampler._matrix;
                newMatrix.preMult(scaleBias[quadrant]);

                // Did something change?
                if (mySampler._texture.get() != parentColorSampler._texture.get() ||
                    mySampler._matrix != newMatrix ||
                    mySampler._revision != parentColorSampler._revision)
                {
                    if (parentColorSampler._texture.valid() && passInLegalRange(parentPass))
                    {
                        // set the parent-color texture to the parent's color texture
                        // and scale/bias the matrix.
                        mySampler._texture = parentColorSampler._texture.get();
                        mySampler._matrix = newMatrix;
                        mySampler._revision = parentColorSampler._revision;
                    }
                    else
                    {
                        // parent has no color texture? Then set our parent-color
                        // equal to our normal color texture.
                        mySampler = myPass->sampler(SamplerBinding::COLOR);
                    }
                    ++changes;
                }
            }
        }
        else
        {
            // Pass exists in the parent node, but not in this node, so add it now.
            if (passInLegalRange(parentPass))
            {
                myPass = &_renderModel.addPass(&parentPass);
                myPass->inheritFrom(parentPass, scaleBias[quadrant]);
                ++changes;
            }
        }
    }

    // Update all the shared samplers (elevation, normal, etc.)
    const Samplers& parentSharedSamplers = parent->_renderModel._sharedSamplers;
    Samplers& mySharedSamplers = _renderModel._sharedSamplers;

    for (unsigned binding=0; binding<parentSharedSamplers.size(); ++binding)
    {
        Sampler& mySampler = mySharedSamplers[binding];

        if (mySampler.inheritsTexture())
        {
            mySampler.inheritFrom(parentSharedSamplers[binding], scaleBias[quadrant]);
            ++changes;

            // Update the local elevation raster cache (for culling and intersection testing).
            if (binding == SamplerBinding::ELEVATION)
            {
                //osg::Image* raster = mySampler._texture.valid() ? mySampler._texture->getImage(0) : NULL;
                //this->setElevationRaster(raster, mySampler._matrix);
                updateElevationRaster();
            }
        }
    }

    if (changes > 0)
    {
        // Bump the data revision for the tile.
        ++_revision;

        dirtyBound(); // only for elev/patch changes maybe?

        if (_childrenReady)
        {
            for (int i = 0; i < 4; ++i)
            {
                TileNode* child = getSubTile(i);
                if (child)
                    child->refreshInheritedData(this, bindings);
            }
        }
    }
}

bool
TileNode::passInLegalRange(const RenderingPass& pass) const
{
    return
        pass.tileLayer() == 0L ||
        pass.tileLayer()->isKeyInVisualRange(getKey());
}

void
TileNode::load(TerrainCuller* culler)
{
    const SelectionInfo& si = _context->getSelectionInfo();
    int lod     = getKey().getLOD();
    int numLods = si.getNumLODs();

    // LOD priority is in the range [0..numLods]d
    float lodPriority = (float)lod;

    // dist priority is in the range [0..1]
    float distance = culler->getDistanceToViewPoint(getBound().center(), true);
    float maxRange = si.getLOD(0)._visibilityRange;
    float distPriority = 1.0 - distance/maxRange;

    // add them together, and you get tiles sorted first by lodPriority
    // (because of the biggest range), and second by distance.
    float priority = lodPriority + distPriority;

    // set atomically
    _loadPriority = priority;

    processLoadQueue(culler);
}

void
TileNode::processLoadQueue(TerrainCuller* culler)
{
    // Check the status of the load
    ScopedMutexLock lock(_loadQueue);

    if (_loadQueue.empty() == false)
    {
        LoadTileDataOperationPtr& op = _loadQueue.front();

        if (op->_result.isAbandoned())
        {
            // Actually this means that the task has not yet been dispatched,
            // so assign the priority and do it now.
            //op->_priority = priority;
            op->dispatch();
        }

        else if (op->_result.isAvailable())
        {
            // The task completed, so submit it to the merger.
            // (We can't merge here in the CULL traversal)
            _context->getMerger()->merge(op, culler);
            _loadQueue.pop();
            _loadsInQueue = _loadQueue.size();
            if (!_loadQueue.empty())
                _nextLoadManifestPtr = &_loadQueue.front()->_manifest;
            else
                _nextLoadManifestPtr = nullptr;
        }
    }
}

void
TileNode::loadSync()
{
    LoadTileDataOperationPtr loadTileData =
        std::make_shared<LoadTileDataOperation>(this, _context.get());

    loadTileData->setEnableCancelation(false);
    loadTileData->dispatch(false);
    loadTileData->merge();
}

bool
TileNode::areSubTilesDormant() const
{
    return
        getNumChildren() >= 4       &&
        getSubTile(0)->isDormant()  &&
        getSubTile(1)->isDormant()  &&
        getSubTile(2)->isDormant()  &&
        getSubTile(3)->isDormant();
}

void
TileNode::removeSubTiles()
{
    _childrenReady = false;
    for(int i=0; i<(int)getNumChildren(); ++i)
    {
        getChild(i)->releaseGLObjects(NULL);
    }
    this->removeChildren(0, this->getNumChildren());

    _createChildResults.clear();
}


void
TileNode::notifyOfArrival(TileNode* that)
{
    if (options().normalizeEdges() == true)
    {
        if (_key.createNeighborKey(1, 0) == that->getKey())
            _eastNeighbor = that;

        if (_key.createNeighborKey(0, 1) == that->getKey())
            _southNeighbor = that;

        updateNormalMap();
    }
}

void
TileNode::updateNormalMap()
{
    if (options().normalizeEdges() == false)
        return;

    Sampler& thisNormalMap = _renderModel._sharedSamplers[SamplerBinding::NORMAL];
    if (thisNormalMap.inheritsTexture() || !thisNormalMap._texture->getImage(0))
        return;

    if (!_eastNeighbor.valid() || !_southNeighbor.valid())
        return;

    osg::ref_ptr<TileNode> east;
    if (_eastNeighbor.lock(east))
    {
        const Sampler& thatNormalMap = east->_renderModel._sharedSamplers[SamplerBinding::NORMAL];
        if (thatNormalMap.inheritsTexture() || !thatNormalMap._texture->getImage(0))
            return;

        osg::Image* thisImage = thisNormalMap._texture->getImage(0);
        osg::Image* thatImage = thatNormalMap._texture->getImage(0);

        int width = thisImage->s();
        int height = thisImage->t();
        if ( width != thatImage->s() || height != thatImage->t() )
            return;

        // Just copy the neighbor's edge normals over to our texture.
        // Averaging them would be more accurate, but then we'd have to
        // re-generate each texture multiple times instead of just once.
        // Besides, there's almost no visual difference anyway.
        osg::Vec4 pixel;
        ImageUtils::PixelReader readThat(thatImage);
        ImageUtils::PixelWriter writeThis(thisImage);

        for (int t=0; t<height; ++t)
        {
            readThat(pixel, 0, t);
            writeThis(pixel, width-1, t);
            //writeThis(readThat(0, t), width-1, t);
        }

        thisImage->dirty();
    }

    osg::ref_ptr<TileNode> south;
    if (_southNeighbor.lock(south))
    {
        const Sampler& thatNormalMap = south->_renderModel._sharedSamplers[SamplerBinding::NORMAL];
        if (thatNormalMap.inheritsTexture() || !thatNormalMap._texture->getImage(0))
            return;

        osg::Image* thisImage = thisNormalMap._texture->getImage(0);
        osg::Image* thatImage = thatNormalMap._texture->getImage(0);

        int width = thisImage->s();
        int height = thisImage->t();
        if ( width != thatImage->s() || height != thatImage->t() )
            return;

        // Just copy the neighbor's edge normals over to our texture.
        // Averaging them would be more accurate, but then we'd have to
        // re-generate each texture multiple times instead of just once.
        // Besides, there's almost no visual difference anyway.
        osg::Vec4 pixel;
        ImageUtils::PixelReader readThat(thatImage);
        ImageUtils::PixelWriter writeThis(thisImage);

        for (int s=0; s<width; ++s)
        {
            readThat(pixel, s, height-1);
            writeThis(pixel, s, 0);
            //writeThis(readThat(s, height-1), s, 0);
        }

        thisImage->dirty();
    }

    //OE_INFO << LC << _key.str() << " : updated normal map.\n";
}

const TerrainOptions&
TileNode::options() const
{
    return _context->options();
}

bool
TileNode::nextLoadIsProgressive() const
{
    return
        (_context->_options.progressive() == true) &&
        (_nextLoadManifestPtr == nullptr) || (!_nextLoadManifestPtr->progressive().isSetTo(false));
}
