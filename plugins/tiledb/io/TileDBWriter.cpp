/******************************************************************************
* Copyright (c) 2019 TileDB, Inc.
*
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following
* conditions are met:
*
*     * Redistributions of source code must retain the above copyright
*       notice, this list of conditions and the following disclaimer.
*     * Redistributions in binary form must reproduce the above copyright
*       notice, this list of conditions and the following disclaimer in
*       the documentation and/or other materials provided
*       with the distribution.
*     * Neither the name of Hobu, Inc. or Flaxen Geo Consulting nor the
*       names of its contributors may be used to endorse or promote
*       products derived from this software without specific prior
*       written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
* "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
* LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
* FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
* COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
* INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
* BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
* OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
* AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
* OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
* OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
* OF SUCH DAMAGE.
****************************************************************************/

#include <string.h>
#include <cctype>
#include <limits>

#include <nlohmann/json.hpp>

#include <pdal/util/FileUtils.hpp>

#include "TileDBWriter.hpp"


const char pathSeparator =
#ifdef _WIN32
        '\\';
#else
        '/';
#endif

namespace pdal {

static PluginInfo const s_info
{
    "writers.tiledb",
    "Write data using TileDB.",
    "http://pdal.io/stages/drivers.tiledb.writer.html"
};

struct TileDBWriter::Args
{
    std::string m_arrayName;
    std::string m_cfgFileName;
    size_t m_tile_capacity;
    size_t m_x_tile_size;
    size_t m_y_tile_size;
    size_t m_z_tile_size;
    size_t m_time_tile_size;
    float m_x_domain_st;
    float m_x_domain_end;
    float m_y_domain_st;
    float m_y_domain_end;
    float m_z_domain_st;
    float m_z_domain_end;
    float m_time_domain_st;
    float m_time_domain_end;
    size_t m_cache_size;
    bool m_stats;
    std::string m_compressor;
    int m_compressionLevel;
    NL::json m_filters;
    NL::json m_defaults;
    bool m_append;
    point_count_t m_timeStamp = 0;
};

CREATE_SHARED_STAGE(TileDBWriter, s_info)

void writeAttributeValue(TileDBWriter::DimBuffer& dim,
    PointRef& point, size_t idx)
{
    Everything e;

    switch (dim.m_type)
    {
    case Dimension::Type::Double:
        e.d = point.getFieldAs<double>(dim.m_id);
        break;
    case Dimension::Type::Float:
        e.f = point.getFieldAs<float>(dim.m_id);
        break;
    case Dimension::Type::Signed8:
        e.s8 = point.getFieldAs<int8_t>(dim.m_id);
        break;
    case Dimension::Type::Signed16:
        e.s16 = point.getFieldAs<int16_t>(dim.m_id);
        break;
    case Dimension::Type::Signed32:
        e.s32 = point.getFieldAs<int32_t>(dim.m_id);
        break;
    case Dimension::Type::Signed64:
        e.s64 = point.getFieldAs<int64_t>(dim.m_id);
        break;
    case Dimension::Type::Unsigned8:
        e.u8 = point.getFieldAs<uint8_t>(dim.m_id);
        break;
    case Dimension::Type::Unsigned16:
        e.u16 = point.getFieldAs<uint16_t>(dim.m_id);
        break;
    case Dimension::Type::Unsigned32:
        e.u32 = point.getFieldAs<uint32_t>(dim.m_id);
        break;
    case Dimension::Type::Unsigned64:
        e.u64 = point.getFieldAs<uint64_t>(dim.m_id);
        break;
    default:
        throw pdal_error("Unsupported attribute type for " + dim.m_name);
    }

    size_t size = Dimension::size(dim.m_type);
    memcpy(dim.m_buffer.data() + (idx * size), &e, size);

}


tiledb::Attribute createAttribute(const tiledb::Context& ctx,
    const std::string name, Dimension::Type t)
{
    switch(t)
    {
        case Dimension::Type::Double:
            return tiledb::Attribute::create<double>(ctx, name);
        case Dimension::Type::Float:
            return tiledb::Attribute::create<float>(ctx, name);
        case Dimension::Type::Signed8:
            return tiledb::Attribute::create<char>(ctx, name);
        case Dimension::Type::Signed16:
            return tiledb::Attribute::create<short>(ctx, name);
        case Dimension::Type::Signed32:
            return tiledb::Attribute::create<int>(ctx, name);
        case Dimension::Type::Signed64:
            return tiledb::Attribute::create<long>(ctx, name);
        case Dimension::Type::Unsigned8:
            return tiledb::Attribute::create<unsigned char>(ctx, name);
        case Dimension::Type::Unsigned16:
            return tiledb::Attribute::create<unsigned short>(ctx, name);
        case Dimension::Type::Unsigned32:
            return tiledb::Attribute::create<unsigned int>(ctx, name);
        case Dimension::Type::Unsigned64:
            return tiledb::Attribute::create<unsigned long>(ctx, name);
        case Dimension::Type::None:
        default:
            throw pdal_error("Unsupported attribute type for " + name);
    }
}


std::unique_ptr<tiledb::Filter> createFilter(const tiledb::Context& ctx, const NL::json& opts)
{
    std::unique_ptr<tiledb::Filter> filter;

    if (!opts.empty())
    {
        std::string name = opts["compression"];

        if (name.empty())
            filter.reset(new tiledb::Filter(ctx, TILEDB_FILTER_NONE));
        else if (name == "gzip")
            filter.reset(new tiledb::Filter(ctx, TILEDB_FILTER_GZIP));
        else if (name == "zstd")
            filter.reset(new tiledb::Filter(ctx, TILEDB_FILTER_ZSTD));
        else if (name == "lz4")
            filter.reset(new tiledb::Filter(ctx, TILEDB_FILTER_LZ4));
        else if (name == "rle")
            filter.reset(new tiledb::Filter(ctx, TILEDB_FILTER_RLE));
        else if (name == "bzip2")
            filter.reset(new tiledb::Filter(ctx, TILEDB_FILTER_BZIP2));
        else if (name == "double-delta")
            filter.reset(new tiledb::Filter(ctx, TILEDB_FILTER_DOUBLE_DELTA));
        else if (name == "bit-width-reduction")
            filter.reset(new tiledb::Filter(ctx, TILEDB_FILTER_BIT_WIDTH_REDUCTION));
        else if (name == "bit-shuffle")
            filter.reset(new tiledb::Filter(ctx, TILEDB_FILTER_BITSHUFFLE));
        else if (name == "byte-shuffle")
            filter.reset(new tiledb::Filter(ctx, TILEDB_FILTER_BYTESHUFFLE));
        else if (name == "positive-delta")
            filter.reset(new tiledb::Filter(ctx, TILEDB_FILTER_POSITIVE_DELTA));
        else
            throw tiledb::TileDBError("Unable to parse compression type: " + name);

        if (opts.count("compression_level") > 0)
            filter->set_option(TILEDB_COMPRESSION_LEVEL, opts["compression_level"].get<int>());

        if (opts.count("bit_width_max_window") > 0)
            filter->set_option(TILEDB_BIT_WIDTH_MAX_WINDOW, opts["bit_width_max_window"].get<int>());

        if (opts.count("positive_delta_max_window") > 0)
            filter->set_option(TILEDB_POSITIVE_DELTA_MAX_WINDOW, opts["positive_delta_max_window"].get<int>());
    }
    else
    {
        filter.reset(new tiledb::Filter(ctx, TILEDB_FILTER_NONE));
    }
    return filter;
}


std::unique_ptr<tiledb::FilterList> createFilterList(const tiledb::Context& ctx, const NL::json& opts)
{
    std::unique_ptr<tiledb::FilterList> filterList(new tiledb::FilterList(ctx));

    if (opts.is_array())
    {
        for (auto& el : opts.items())
        {
            auto v = el.value();
            filterList->add_filter(*createFilter(ctx, v));
        }
    }
    else
    {
        filterList->add_filter(*createFilter(ctx, opts));
    }
    return filterList;
}


TileDBWriter::TileDBWriter(): 
    m_args(new TileDBWriter::Args)
{
    std::string attributeDefaults(R"(
    {
        "coords":[
            {"compression": "zstd", "compression_level": 7}
        ],
        "Intensity":{"compression": "bzip2", "compression_level": 5},
        "ReturnNumber": {"compression": "zstd", "compression_level": 7},
        "NumberOfReturns": {"compression": "zstd", "compression_level": 7},
        "ScanDirectionFlag": {"compression": "bzip2", "compression_level": 5},
        "EdgeOfFlightLine": {"compression": "bzip2", "compression_level": 5},
        "Classification": {"compression": "gzip", "compression_level": 9},
        "ScanAngleRank": {"compression": "bzip2", "compression_level": 5},
        "UserData": {"compression": "gzip", "compression_level": 9},
        "PointSourceId": {"compression": "bzip2"},
        "Red": {"compression": "zstd", "compression_level": 7},
        "Green": {"compression": "zstd", "compression_level": 7},
        "Blue": {"compression": "zstd", "compression_level": 7},
        "GpsTime": [
        {"compression": "zstd", "compression_level": 7}
        ]
    })");

    m_args->m_defaults = NL::json::parse(attributeDefaults);
}


TileDBWriter::~TileDBWriter(){}


std::string TileDBWriter::getName() const { return s_info.name; }


void TileDBWriter::addArgs(ProgramArgs& args)
{
    args.add("array_name", "TileDB array name",
        m_args->m_arrayName).setPositional();
    args.addSynonym("array_name", "filename");
    args.add("config_file", "TileDB configuration file location",
        m_args->m_cfgFileName);
    args.add("data_tile_capacity", "TileDB tile capacity",
        m_args->m_tile_capacity, size_t(100000));
    args.add("x_tile_size", "TileDB tile size", m_args->m_x_tile_size,
        size_t(0));
    args.add("y_tile_size", "TileDB tile size", m_args->m_y_tile_size,
        size_t(0));
    args.add("z_tile_size", "TileDB tile size", m_args->m_z_tile_size,
        size_t(0));
    args.add("time_tile_size", "TileDB tile size", m_args->m_time_tile_size,
             size_t(0));
    args.add("x_domain_st", "TileDB start of domain in X", m_args->m_x_domain_st, 0.f);
    args.add("x_domain_end", "TileDB end of domain in X", m_args->m_x_domain_end, 0.f);
    args.add("y_domain_st", "TileDB start of domain in Y", m_args->m_y_domain_st, 0.f);
    args.add("y_domain_end", "TileDB end of domain in Y", m_args->m_y_domain_end, 0.f);
    args.add("z_domain_st", "TileDB start of domain in Z", m_args->m_z_domain_st, 0.f);
    args.add("z_domain_end", "TileDB end of domain in Z", m_args->m_z_domain_end, 0.f);
    args.add("time_domain_st", "TileDB start of domain in GpsTime", m_args->m_time_domain_st, 0.f);
    args.add("time_domain_end", "TileDB end of domain in GpsTime", m_args->m_time_domain_end, 0.f);
    args.add("chunk_size", "Point cache size for chunked writes",
        m_args->m_cache_size, size_t(10000));
    args.add("stats", "Dump TileDB query stats to stdout", m_args->m_stats,
        false);
    args.add("compression", "TileDB compression type for attributes",
        m_args->m_compressor);
    args.add("compression_level", "TileDB compression level",
        m_args->m_compressionLevel, -1);
    args.add("filters", "Specify filter and level per dimension/attribute",
        m_args->m_filters, NL::json({}));
    args.add("append", "Append to existing TileDB array",
        m_args->m_append, false);
    args.add("use_time_dim", "Use GpsTime coordinate data as array dimension", m_use_time, false);
    args.addSynonym("use_time_dim", "use_time");
    args.add("time_first", "If writing 4D array with XYZ and Time, choose to put time dim first or last (default)", m_time_first, false);
#if TILEDB_VERSION_MAJOR >= 2
    args.add("timestamp", "TileDB array timestamp", m_args->m_timeStamp,
        point_count_t(0));
#endif
}


void TileDBWriter::initialize()
{
    try
    {
        if (!m_args->m_cfgFileName.empty())
        {
            tiledb::Config cfg(m_args->m_cfgFileName);
            m_ctx.reset(new tiledb::Context(cfg));
        }
        else
            m_ctx.reset(new tiledb::Context());

        if (!m_args->m_append)
        {
            NL::json opts;

            m_schema.reset(new tiledb::ArraySchema(*m_ctx, TILEDB_SPARSE));

            if (m_args->m_filters.count("coords") > 0)
            {
                opts = m_args->m_filters["coords"];
            }
            else if (!m_args->m_compressor.empty())
            {
                opts["compression"] = m_args->m_compressor;
                opts["compression_level"] = m_args->m_compressionLevel;
            }
            else
            {
                opts = m_args->m_defaults["coords"];
            }
#if TILEDB_VERSION_MAJOR > 1
            m_schema->set_allows_dups(true);
#endif
            m_schema->set_coords_filter_list(
                *createFilterList(*m_ctx, opts));
        }
    }
    catch (const tiledb::TileDBError& err)
    {
        throwError(std::string("TileDB Error: ") + err.what());
    }
}


void TileDBWriter::ready(pdal::BasePointTable &table)
{
    auto layout = table.layout();
    auto all = layout->dims();
    MetadataNode m = table.metadata();

    m = m.findChild("filters.stats:bbox:native:bbox");

    if (m_args->m_stats)
        tiledb::Stats::enable();

    // get a list of all the dimensions & their types and add to schema
    // x,y,z will be tiledb dimensions other pdal dimensions will be
    // tiledb attributes
    if (!m_args->m_append)
    {
        tiledb::Domain domain(*m_ctx);
        double dimMin = std::numeric_limits<double>::lowest();
        double dimMax = std::numeric_limits<double>::max();

        if ( (m_args->m_x_tile_size > 0) &&
             (m_args->m_y_tile_size > 0) &&
             (m_args->m_z_tile_size > 0) &&
             (!m_use_time || m_args->m_time_tile_size > 0))
        {
            if (isValidDomain(*m_args))
            {
                if (m_use_time && m_time_first)
                {
                    domain.add_dimension(tiledb::Dimension::create<double>(*m_ctx, "GpsTime",
                       {{m_args->m_time_domain_st, m_args->m_time_domain_end}}, m_args->m_time_tile_size));
                }
                domain.add_dimension(tiledb::Dimension::create<double>(*m_ctx, "X",
                    {{m_args->m_x_domain_st, m_args->m_x_domain_end}}, m_args->m_x_tile_size))
                    .add_dimension(tiledb::Dimension::create<double>(*m_ctx, "Y",
                    {{m_args->m_y_domain_st, m_args->m_y_domain_end}}, m_args->m_y_tile_size))
                    .add_dimension(tiledb::Dimension::create<double>(*m_ctx, "Z",
                    {{m_args->m_z_domain_st, m_args->m_z_domain_end}}, m_args->m_z_tile_size));
                if (m_use_time && !m_time_first) {
                    domain.add_dimension(tiledb::Dimension::create<double>(*m_ctx, "GpsTime",
                        {{m_args->m_time_domain_st, m_args->m_time_domain_end}}, m_args->m_time_tile_size));
                }
            }
            else
            {
                // read from table.metadata and if not available then use dimMin, dimMax
                if (m.valid())
                {
                    if (m_use_time && m_time_first) {
                        domain.add_dimension(tiledb::Dimension::create<double>(*m_ctx, "GpsTime",
                            {{m.findChild("mintm").value<double>() - 1, m.findChild("maxtm").value<double>() + 1}},
                             m_args->m_time_tile_size));
                    }
                    domain.add_dimension(tiledb::Dimension::create<double>(*m_ctx, "X",
                            {{m.findChild("minx").value<double>() - 1., m.findChild("maxx").value<double>() + 1.}},
                            m_args->m_x_tile_size))
                        .add_dimension(tiledb::Dimension::create<double>(*m_ctx, "Y",
                            {{m.findChild("miny").value<double>() - 1., m.findChild("maxy").value<double>() + 1.}},
                            m_args->m_y_tile_size))
                        .add_dimension(tiledb::Dimension::create<double>(*m_ctx, "Z",
                            {{m.findChild("minz").value<double>() - 1., m.findChild("maxz").value<double>() + 1.}},
                            m_args->m_z_tile_size));
                    if (m_use_time && !m_time_first) {
                        domain.add_dimension(tiledb::Dimension::create<double>(*m_ctx, "GpsTime",
                           {{m.findChild("mintm").value<double>() - 1, m.findChild("maxtm").value<double>() + 1}},
                           m_args->m_time_tile_size));
                    }
                }
                else
                {
                   if (m_use_time && m_time_first) {
                        domain.add_dimension(tiledb::Dimension::create<double>(*m_ctx, "GpsTime",
                            {{dimMin, dimMax}}, m_args->m_time_tile_size));
                   }
                   domain.add_dimension(tiledb::Dimension::create<double>(*m_ctx, "X",
                            {{dimMin, dimMax}},m_args->m_x_tile_size))
                        .add_dimension(tiledb::Dimension::create<double>(*m_ctx, "Y",
                            {{dimMin, dimMax}}, m_args->m_y_tile_size))
                        .add_dimension(tiledb::Dimension::create<double>(*m_ctx, "Z",
                            {{dimMin, dimMax}}, m_args->m_z_tile_size));
                   if (m_use_time && !m_time_first) {
                       domain.add_dimension(tiledb::Dimension::create<double>(*m_ctx, "GpsTime",
                          {{dimMin, dimMax}}, m_args->m_time_tile_size));
                   }
                }
            }
        }
#if TILEDB_VERSION_MAJOR >= 2
    #if ((TILEDB_VERSION_MINOR > 1) || (TILEDB_VERSION_MAJOR > 2))
        else
        {
            if (isValidDomain(*m_args))
            {
                if (m_use_time && m_time_first) {
                    domain.add_dimension(tiledb::Dimension::create<double>(*m_ctx, "GpsTime",
                    {{m_args->m_time_domain_st, m_args->m_time_domain_end}}));
                }
                domain.add_dimension(tiledb::Dimension::create<double>(*m_ctx, "X",
                    {{m_args->m_x_domain_st, m_args->m_x_domain_end}}))
                    .add_dimension(tiledb::Dimension::create<double>(*m_ctx, "Y",
                    {{m_args->m_y_domain_st, m_args->m_y_domain_end}}))
                    .add_dimension(tiledb::Dimension::create<double>(*m_ctx, "Z",
                    {{m_args->m_z_domain_st, m_args->m_z_domain_end}}));
                if (m_use_time && !m_time_first) {
                    domain.add_dimension(tiledb::Dimension::create<double>(*m_ctx, "GpsTime",
                    {{m_args->m_time_domain_st, m_args->m_time_domain_end}}));
                }
            }
            else
            {
                // read from table.metadata and if not available then throw error
                if (m.valid())
                {
                    if (m_use_time && m_time_first) {
                        domain.add_dimension(tiledb::Dimension::create<double>(*m_ctx, "GpsTime",
                            {{m.findChild("mintm").value<double>() - 1,m.findChild("maxtm").value<double>() + 1}}));
                    }
                    domain.add_dimension(tiledb::Dimension::create<double>(*m_ctx, "X",
                            {{m.findChild("minx").value<double>() - 1., m.findChild("maxx").value<double>() + 1.}}))
                        .add_dimension(tiledb::Dimension::create<double>(*m_ctx, "Y",
                            {{m.findChild("miny").value<double>() - 1., m.findChild("maxy").value<double>() + 1.}}))
                        .add_dimension(tiledb::Dimension::create<double>(*m_ctx, "Z",
                            {{m.findChild("minz").value<double>() - 1., m.findChild("maxz").value<double>() + 1.}}));
                    if (m_use_time && !m_time_first) {
                        domain.add_dimension(tiledb::Dimension::create<double>(*m_ctx, "GpsTime",
                           {{m.findChild("mintm").value<double>() - 1,m.findChild("maxtm").value<double>() + 1}}));
                    }
                }
                else
                {
                    throwError("Using TileDB Hilbert ordering, must specify a domain extent or execute a prior stats filter stage."); 
                }
            }
            m_schema->set_cell_order(TILEDB_HILBERT);
        }
    #endif
#endif
        m_schema->set_domain(domain);
        m_schema->set_capacity(m_args->m_tile_capacity);
    }
    else
    {
        if (m_args->m_timeStamp)
            m_array.reset(new tiledb::Array(*m_ctx, m_args->m_arrayName,
                TILEDB_WRITE, m_args->m_timeStamp));
        else
            m_array.reset(new tiledb::Array(*m_ctx, m_args->m_arrayName,
                TILEDB_WRITE));
        if (m_array->schema().domain().has_dimension("GpsTime"))
            m_use_time = true;
    }

    for (const auto& d : all)
    {
        std::string dimName = layout->dimName(d);

        if ((dimName != "X") && (dimName != "Y") && (dimName != "Z") &&
            ((m_use_time && dimName != "GpsTime") || !m_use_time))
        {
            Dimension::Type type = layout->dimType(d);
            if (!m_args->m_append)
            {
                NL::json opts;
                tiledb::Attribute att = createAttribute(*m_ctx, dimName, type);
                if (m_args->m_filters.count(dimName) > 0)
                {
                    opts = m_args->m_filters[dimName];
                }
                else if (!m_args->m_compressor.empty())
                {
                    opts["compression"] = m_args->m_compressor;
                    opts["compression_level"] = m_args->m_compressionLevel;
                }
                else
                {
                    if (m_args->m_defaults.count(dimName) > 0)
                        opts = m_args->m_defaults[dimName];
                }

                if (!opts.empty())
                    att.set_filter_list(
                        *createFilterList(*m_ctx, opts));

                m_schema->add_attribute(att);
            }
            else
            {
                // check attribute exists in original tiledb array
                auto attrs = m_array->schema().attributes();
                auto it = attrs.find(dimName);
                if (it == attrs.end())
                    throwError("Attribute " + dimName +
                        " does not exist in original array.");
            }
            
            m_attrs.emplace_back(dimName, d, type);
            // Size the buffers.
            m_attrs.back().m_buffer.resize(
                m_args->m_cache_size * Dimension::size(type));
        }
    }

    if (!m_args->m_append)
    {
        tiledb::Array::create(m_args->m_arrayName, *m_schema);

        if (m_args->m_timeStamp)
            m_array.reset(new tiledb::Array(*m_ctx, m_args->m_arrayName,
                TILEDB_WRITE, m_args->m_timeStamp));
        else
            m_array.reset(new tiledb::Array(*m_ctx, m_args->m_arrayName,
                TILEDB_WRITE));
    }

    m_current_idx = 0;
}


bool TileDBWriter::processOne(PointRef& point)
{

    double x = point.getFieldAs<double>(Dimension::Id::X);
    double y = point.getFieldAs<double>(Dimension::Id::Y);
    double z = point.getFieldAs<double>(Dimension::Id::Z);
    double tm(0);
    if (m_use_time)
        tm = point.getFieldAs<double>(Dimension::Id::GpsTime);

    for (auto& a : m_attrs)
        writeAttributeValue(a, point, m_current_idx);

    m_xs.push_back(x);
    m_ys.push_back(y);
    m_zs.push_back(z);
    if (m_use_time)
        m_tms.push_back(tm);

    if (++m_current_idx == m_args->m_cache_size)
    {
        if (!flushCache(m_current_idx))
        {
            throwError("Unable to flush points to TileDB array");
        }
    }

    return true;
}


void TileDBWriter::write(const PointViewPtr view)
{
    PointRef point(*view, 0);
    for (PointId idx = 0; idx < view->size(); ++idx)
    {
        point.setPointId(idx);
        processOne(point);
    }
}


void TileDBWriter::done(PointTableRef table)
{
    if (flushCache(m_current_idx))
    {
        if (!m_args->m_append)
        {
            // write pipeline metadata sidecar inside array
            MetadataNode node = getMetadata();
            if (!getSpatialReference().empty() && table.spatialReferenceUnique())
            {
                // The point view takes on the spatial reference of that stage,
                // if it had one.
                node.add("spatialreference", 
                    Utils::toString(getSpatialReference()));
            }

            // serialize metadata
#if TILEDB_VERSION_MAJOR == 1 && TILEDB_VERSION_MINOR < 7
            tiledb::VFS vfs(*m_ctx, m_ctx->config());
            tiledb::VFS::filebuf fbuf(vfs);

            if (vfs.is_dir(m_args->m_arrayName))
                fbuf.open(m_args->m_arrayName + pathSeparator + "pdal.json",
                    std::ios::out);
            else
            {
                std::string fname = m_args->m_arrayName + "/pdal.json";
                vfs.touch(fname);
                fbuf.open(fname, std::ios::out);
            }

            std::ostream os(&fbuf);

            if (!os.good())
                throwError("Unable to create sidecar file for " +
                    m_args->m_arrayName);

            pdal::Utils::toJSON(node, os);

            fbuf.close();
#else
            std::string m = pdal::Utils::toJSON(node);
            m_array->put_metadata("_pdal", TILEDB_UINT8, m.length() + 1, m.c_str());
#endif
        }
        m_array->close();
    }
    else{
        throwError("Unable to flush points to TileDB array");
    }
}


bool TileDBWriter::isValidDomain(TileDBWriter::Args& args)
{
    return ((( args.m_x_domain_end  - args.m_x_domain_st ) > 0) &&
            (( args.m_y_domain_end  - args.m_y_domain_st ) > 0) &&
            (( args.m_z_domain_end  - args.m_z_domain_st ) > 0));
}


bool TileDBWriter::flushCache(size_t size)
{
    tiledb::Query query(*m_ctx, *m_array);
    query.set_layout(TILEDB_UNORDERED);

#if TILEDB_VERSION_MAJOR == 1
    // backwards compatibility requires a copy
    std::vector<double> coords;

    for(unsigned i = 0; i < m_xs.size(); i++)
    {
        coords.push_back(m_xs[i]);
        coords.push_back(m_ys[i]);
        coords.push_back(m_zs[i]);
        if (m_use_time)
            coords.push_back(m_tms[i]);
    }
    query.set_coordinates(coords);
#else
    query.set_buffer("X", m_xs);
    query.set_buffer("Y", m_ys);
    query.set_buffer("Z", m_zs);
    if (m_use_time)
        query.set_buffer("GpsTime", m_tms);
#endif

    // set tiledb buffers
    for (const auto& a : m_attrs)
    {
        uint8_t *buf = const_cast<uint8_t *>(a.m_buffer.data());
        switch (a.m_type)
        {
        case Dimension::Type::Double:
            query.set_buffer(a.m_name, reinterpret_cast<double *>(buf),
                size);
            break;
        case Dimension::Type::Float:
            query.set_buffer(a.m_name, reinterpret_cast<float *>(buf),
                size);
            break;
        case Dimension::Type::Signed8:
            query.set_buffer(a.m_name, reinterpret_cast<int8_t *>(buf),
                size);
            break;
        case Dimension::Type::Signed16:
            query.set_buffer(a.m_name, reinterpret_cast<int16_t *>(buf),
                size);
            break;
        case Dimension::Type::Signed32:
            query.set_buffer(a.m_name, reinterpret_cast<int32_t *>(buf),
                size);
            break;
        case Dimension::Type::Signed64:
            query.set_buffer(a.m_name, reinterpret_cast<int64_t *>(buf),
                size);
            break;
        case Dimension::Type::Unsigned8:
            query.set_buffer(a.m_name, reinterpret_cast<uint8_t *>(buf),
                size);
            break;
        case Dimension::Type::Unsigned16:
            query.set_buffer(a.m_name, reinterpret_cast<uint16_t *>(buf),
                size);
            break;
        case Dimension::Type::Unsigned32:
            query.set_buffer(a.m_name, reinterpret_cast<uint32_t *>(buf),
                size);
            break;
        case Dimension::Type::Unsigned64:
            query.set_buffer(a.m_name, reinterpret_cast<uint64_t *>(buf),
                size);
            break;
        case Dimension::Type::None:
        default:
            throw pdal_error("Unsupported attribute type for " + a.m_name);
        }
    }


    tiledb::Query::Status status = query.submit();

    if (m_args->m_stats)
    {
        tiledb::Stats::dump(stdout);
        tiledb::Stats::reset();
    }

    m_current_idx = 0;
    m_xs.clear();
    m_ys.clear();
    m_zs.clear();
    m_tms.clear();

    if (status == tiledb::Query::Status::FAILED)
        return false;
    else
        return true;
}

} // namespace pdal
