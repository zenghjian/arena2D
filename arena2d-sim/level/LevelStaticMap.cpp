#include "LevelStaticMap.hpp"

std::shared_ptr<nav_msgs::OccupancyGrid> StaticMap::m_static_map = nullptr;
std::unique_ptr<ros::NodeHandle> StaticMap::m_nh = nullptr;

LevelStaticMap::LevelStaticMap(const LevelDef &d, bool dynamic = false) : Level(d), _dynamic(dynamic), _init_reset(true)
{
    _n_non_clear_bodies = 0;
    _occupancygrid_ptr = StaticMap::getMap(_SETTINGS->stage.static_map_ros_service_name);
    ROS_DEBUG("load map start");
    loadStaticMap();
    ROS_DEBUG("loaded map!");
}

void LevelStaticMap::reset(bool robot_position_reset)
{
    ROS_DEBUG("reset start!");
    lazyclear();
    if (_dynamic)
        freeWanderers();
    if (robot_position_reset)
    {
        resetRobotToCenter();
    }
    const float half_goal_size = _SETTINGS->stage.goal_size / 2.f;
    const float dynamic_radius = _SETTINGS->stage.dynamic_obstacle_size / 2.f;
    const float dynamic_speed = _SETTINGS->stage.obstacle_speed;
    const int num_obstacles = _SETTINGS->stage.num_obstacles;
    const int num_dynamic_obstacles = _SETTINGS->stage.num_dynamic_obstacles;
    const float min_obstacle_radius = _SETTINGS->stage.min_obstacle_size / 2;
    const float max_obstacle_radius = _SETTINGS->stage.max_obstacle_size / 2;
    const auto &info = _occupancygrid_ptr->info;
    const float half_height = info.resolution * info.height / 2;
    const float half_width = info.resolution * info.width / 2;
    const zRect main_rect(0, 0, half_width, half_height);
    const zRect big_main_rect(0, 0, half_width + max_obstacle_radius, half_height + max_obstacle_radius);

    if (_init_reset)
    {
        // calculating goal spawn area
        _goalSpawnArea.addQuadTree(main_rect, _levelDef.world, COLLIDE_CATEGORY_STAGE,
                                   LEVEL_RANDOM_GOAL_SPAWN_AREA_BLOCK_SIZE, half_goal_size);
        _goalSpawnArea.calculateArea();
    }

    // dynamic obstacles
    if (_dynamic)
    {
        //
        if (_init_reset)
        {
            ROS_INFO("calculating the respawn area for dynamic obstacles, it may take a while and the GUI is in black "
                     "screen...");
            _dynamicSpawn.clear();
            _dynamicSpawn.addCheeseRect(main_rect, _levelDef.world, COLLIDE_CATEGORY_STAGE | COLLIDE_CATEGORY_PLAYER,
                                        dynamic_radius);
            _dynamicSpawn.calculateArea();
            ROS_INFO("calculation the respawn area for dynamic obstacles is done");
            _init_reset = false;
        }
        for (int i = 0; i < num_dynamic_obstacles; i++)
        {
            b2Vec2 p;
            _dynamicSpawn.getRandomPoint(p);
            Wanderer *w = new Wanderer(_levelDef.world, p, dynamic_speed, 0.1, 0.05);
            w->addCircle(dynamic_radius);
            _wanderers.push_back(w);
        }
    }
    ROS_DEBUG("dynamic obstacles created!");
    randomGoalSpawnUntilValid();
    ROS_DEBUG("goal spawned");
}

void LevelStaticMap::freeWanderers()
{
    for (std::list<Wanderer *>::iterator it = _wanderers.begin(); it != _wanderers.end(); it++)
    {
        delete (*it);
    }
    _wanderers.clear();
}

void LevelStaticMap::update()
{
    for (std::list<Wanderer *>::iterator it = _wanderers.begin(); it != _wanderers.end(); it++)
    {
        (*it)->update();
    }
}

void LevelStaticMap::renderGoalSpawn()
{
    Level::renderGoalSpawn();
    Z_SHADER->setColor(zColor(0.1, 0.9, 0.0, 0.5));
    _dynamicSpawn.render();
}

void LevelStaticMap::loadStaticMap(bool enable_line_approximation)
{
    b2Assert(_occupancygrid_ptr);
    const auto &info = _occupancygrid_ptr->info;
    const auto &data = _occupancygrid_ptr->data;
    uint32 cols = info.width;
    uint32 rows = info.height;
    float resolution = info.resolution;
    // get the position of the left-upper cell, we assume the origin of the coordinate system is the center of the map
    b2Vec2 lower_left_pos(-((cols >> 1) - ((cols & 1) ^ 1) / 2.f) * resolution,
                          -((rows >> 1) - ((rows & 1) ^ 1) / 2.f) * resolution);
    // b2Vec2 lower_left_pos(info.origin.position.x, info.origin.position.y);
    // if we dont approxiamte the occupancy grid cells with line segment, each cell will be represented with
    // a singe box, which is not efficiency.
    if (!enable_line_approximation)
    {
        for (int i = 0; i < rows; i++)
        {
            for (int j = 0; j < cols; j++)
            {
                // in occupid grid , 100->occupied, 0->unoccupied, -1->unknown
                if (data[i * cols + j] == 100 || data[i * cols + j] == -1)
                {
                    addBox(lower_left_pos + b2Vec2(j * resolution, i * resolution), resolution / 2, resolution / 2, 0);
                    _n_non_clear_bodies++;
                }
            }
        }
    }
    else
    {
        b2BodyDef b;
        b.type = b2_staticBody;
        b2Body *body = _levelDef.world->CreateBody(&b);
        // unit: pixel
        auto add_edge = [&](double x1, double y1, double x2, double y2) {
            b2EdgeShape edge;

            edge.Set(b2Vec2(lower_left_pos.x + resolution * x1, lower_left_pos.y + resolution * y1),
                     b2Vec2(lower_left_pos.x + resolution * x2, lower_left_pos.y + resolution * y2));

            b2FixtureDef fixture_def;
            fixture_def.shape = &edge;
            fixture_def.filter.categoryBits = COLLIDE_CATEGORY_STAGE;
            fixture_def.friction = LEVEL_STATIC_FRICTION;
            fixture_def.restitution = LEVEL_STATIC_RESTITUTION;
            body->CreateFixture(&fixture_def);
        };

        cv::Mat static_map(rows, cols, CV_8UC1);
        for (int i = 0; i < rows; i++)
        {
            for (int j = 0; j < cols; j++)
            {
                static_map.at<uint8>(i, j) = (data[i * cols + j] == 100 || data[i * cols + j] == -1) ? 255 : 0;
            }
        }
        // Create FLD detector
        // Param               Default value   Description
        // length_threshold    10            - Segments shorter than this will be discarded
        // distance_threshold  1.41421356    - A point placed from a hypothesis line
        //                                     segment farther than this will be
        //                                     regarded as an outlier
        // canny_th1           50            - First threshold for
        //                                     hysteresis procedure in Canny()
        // canny_th2           50            - Second threshold for
        //                                     hysteresis procedure in Canny()
        // canny_aperture_size 3             - Aperturesize for the sobel
        //                                     operator in Canny()
        // do_merge            false         - If true, incremental merging of segments
        //                                     will be perfomred
        // int length_threshold = 1;
        // float distance_threshold = 1.41421356f;
        // double canny_th1 = 0.1;
        // double canny_th2 = 0.2;
        // int canny_aperture_size = 3;
        // bool do_merge = false;
        // auto fld = cv::ximgproc::createFastLineDetector(length_threshold,
        //                                                 distance_threshold, canny_th1, canny_th2, canny_aperture_size,
        //                                                 do_merge);
        // vector<cv::Vec4f> lines_fld;
        // fld->detect(static_map, lines_fld);
        // for (const auto &line : lines_fld)
        // {
        //     add_edge(line[0], line[1], line[2], line[3]);
        // }

        // loop through all the rows, looking at 2 at once
        for (int i = 0; i < static_map.rows - 1; i++)
        {
            cv::Mat row1 = static_map.row(i);
            cv::Mat row2 = static_map.row(i + 1);
            cv::Mat diff;

            // if the two row are the same value, there is no edge
            // if the two rows are not the same value, there is an edge
            // result is still binary, either 255 or 0
            cv::absdiff(row1, row2, diff);

            int start = 0;
            bool started = false;

            // find all the walls, put the connected walls as a single line segment
            for (unsigned int j = 0; j <= diff.total(); j++)
            {
                bool edge_exists = false;
                if (j < diff.total())
                {
                    edge_exists = diff.at<uint8_t>(0, j); // 255 maps to true
                }

                if (edge_exists && !started)
                {
                    start = j;
                    started = true;
                }
                else if (started && !edge_exists)
                {
                    add_edge(start, i, j, i);
                    // add_edge(start, i + 1, j, i + 1);
                    started = false;
                }
            }
        }
        // loop through all the columns, looking at 2 at once
        for (int i = 0; i < static_map.cols - 1; i++)
        {
            cv::Mat col1 = static_map.col(i);
            cv::Mat col2 = static_map.col(i + 1);
            cv::Mat diff;

            cv::absdiff(col1, col2, diff);

            int start = 0;
            bool started = false;

            for (unsigned int j = 0; j <= diff.total(); j++)
            {
                bool edge_exists = false;
                if (j < diff.total())
                {
                    edge_exists = diff.at<uint8_t>(j, 0);
                }

                if (edge_exists && !started)
                {
                    start = j;
                    started = true;
                }
                else if (started && !edge_exists)
                {
                    add_edge(i, start, i, j);
                    // add_edge(i + 1, start, i + 1, j);

                    started = false;
                }
            }
        }
        _n_non_clear_bodies++;
        _bodyList.push_back(body);
    }
}
void LevelStaticMap::lazyclear()
{
    // only free the bodies that not belong to static map.
    int i = 0, size = _bodyList.size();
    for (auto it = _bodyList.begin(); it != _bodyList.end();)
    {
        if (i++ > _n_non_clear_bodies)
        {
            _levelDef.world->DestroyBody(*it);
            it = _bodyList.erase(it);
        }
        else
        {
            it++;
        }
    }
}