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
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */
#include <osgEarth/TiledFeatureModelGraph>
#include <osgEarth/GeometryCompiler>
#include <osgEarth/FeatureModelSource>
#include <osgEarth/NetworkMonitor>
#include <osgEarth/Registry>

using namespace osgEarth;

TiledFeatureModelGraph::TiledFeatureModelGraph(const osgEarth::Map* map,
                                               FeatureSource* features,
                                               StyleSheet* styleSheet,
                                               Session* session) :
    SimplePager(map, (features && features->getFeatureProfile()) ? features->getFeatureProfile()->getTilingProfile() : nullptr),
    _features(features),
    _styleSheet(styleSheet),
    _session(session)
{
    setMinLevel(features->getFeatureProfile()->getFirstLevel());
    setMaxLevel(features->getFeatureProfile()->getMaxLevel());

    _session->setResourceCache(new ResourceCache());


    FeatureSourceIndexOptions indexOptions;
    indexOptions.enabled() = true;

    _featureIndex = new FeatureSourceIndex(
        features,
        Registry::objectIndex(),
        indexOptions);
}

void
TiledFeatureModelGraph::setFilterChain(const FeatureFilterChain& chain)
{
    _filterChain = chain;
}

void
TiledFeatureModelGraph::setOwnerName(const std::string& value)
{
    _ownerName = value;
}


FeatureCursor
TiledFeatureModelGraph::createCursor(FeatureSource* fs, FilterContext& cx, const Query& query, ProgressCallback* progress) const
{
    NetworkMonitor::ScopedRequestLayer layerRequest(_ownerName);
    auto cursor = fs->createFeatureCursor(query, progress);
    if (!_filterChain.empty())
    {
        cursor.set(new FilteredFeatureCursorImpl(cursor, _filterChain, &cx));
    }
    return cursor;
}

osg::ref_ptr<osg::Node>
TiledFeatureModelGraph::createNode(const TileKey& key, ProgressCallback* progress)
{
    if (progress && progress->isCanceled())
        return nullptr;

    NetworkMonitor::ScopedRequestLayer layerRequest(_ownerName);
    // Get features for this key
    Query query;
    query.tileKey() = key;

    GeoExtent dataExtent = key.getExtent();

    // set up for feature indexing if appropriate:
    osg::ref_ptr< FeatureSourceIndexNode > index = 0L;

    if (_featureIndex.valid())
    {
        index = new FeatureSourceIndexNode(_featureIndex.get());
    }

    FilterContext fc(_session.get(), new FeatureProfile(dataExtent), dataExtent, index);

    GeometryCompilerOptions options;
    options.instancing() = true;
    //options.mergeGeometry() = true;
    GeometryCompiler gc(options);

    GeomFeatureNodeFactory factory(options);

    if (progress && progress->isCanceled())
        return nullptr;

    auto cursor = _features->createFeatureCursor(
        query,
        _filterChain,
        &fc,
        progress);

    osg::ref_ptr<osg::Node> node = new osg::Group;
    if (cursor.hasMore())
    {
        if (progress && progress->isCanceled())
            return nullptr;

        FeatureList features;
        cursor.fill(features);

        if (_styleSheet->getSelectors().size() > 0)
        {
            osg::Group* group = new osg::Group;

            for (StyleSelectors::const_iterator i = _styleSheet->getSelectors().begin();
                i != _styleSheet->getSelectors().end();
                ++i)
            {
                typedef std::map< std::string, FeatureList > StyleToFeaturesMap;
                StyleToFeaturesMap styleToFeatures;

                // pull the selected style...
                const StyleSelector& sel = i->second;

                if (sel.styleExpression().isSet())
                {
                    // establish the working bounds and a context:
                    StringExpression styleExprCopy(sel.styleExpression().get());

                    for(auto& feature : features)
                    {
                        //feature->set("level", (long long)key.getLevelOfDetail());

                        const std::string& styleString = feature->eval(styleExprCopy, &fc);
                        if (!styleString.empty() && styleString != "null")
                        {
                            styleToFeatures[styleString].push_back(feature);
                        }

                        if (progress && progress->isCanceled())
                            return nullptr;
                    }
                }

                std::unordered_map<std::string, Style> literal_styles;

                for (StyleToFeaturesMap::iterator itr = styleToFeatures.begin(); itr != styleToFeatures.end(); ++itr)
                {
                    const std::string& styleString = itr->first;
                    Style* style = nullptr;

                    if (styleString.length() > 0 && styleString[0] == '{')
                    {
                        Config conf("style", styleString);
                        conf.setReferrer(sel.styleExpression().get().uriContext().referrer());
                        conf.set("type", "text/css");
                        Style& literal_style = literal_styles[conf.toJSON()];
                        if (literal_style.empty())
                            literal_style = Style(conf);
                        style = &literal_style;
                    }
                    else
                    {
                        style = _styleSheet->getStyle(styleString);
                    }

                    if (style)
                    {
                        osg::Group* styleGroup = factory.getOrCreateStyleGroup(*style, _session.get());
                        osg::ref_ptr< osg::Node>  styleNode;
                        FeatureCursor cursor(itr->second);
                        Query query;
                        factory.createOrUpdateNode(cursor, *style, fc, styleNode, query);
                        if (styleNode.valid())
                        {
                            styleGroup->addChild(styleNode);
                            if (!group->containsNode(styleGroup))
                            {
                                group->addChild(styleGroup);
                            }
                        }
                    }
                }
            }

            
            node = group;
        }
        else if (_styleSheet->getDefaultStyle())
        {
            FeatureCursor cursor(features);
            osg::ref_ptr< osg::Group > group = new osg::Group;
            osg::ref_ptr< osg::Group > styleGroup = factory.getOrCreateStyleGroup(*_styleSheet->getDefaultStyle(), _session.get());
            osg::ref_ptr< osg::Node>  styleNode;
            factory.createOrUpdateNode(cursor, *_styleSheet->getDefaultStyle(), fc, styleNode, query);
            if (styleNode.valid())
            {
                group->addChild(styleGroup);
                styleGroup->addChild(styleNode);
                node = group;
            }
        }
    }

    if (!node->getBound().valid())
    {
        return nullptr;
    }

    if (index.valid())
    {
        index->addChild(node);
        return index;
    }

    return node;
}