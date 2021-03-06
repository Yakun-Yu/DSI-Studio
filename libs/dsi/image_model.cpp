#include <QFileInfo>
#include "image_model.hpp"
#include "odf_process.hpp"
#include "dti_process.hpp"

void ImageModel::calculate_dwi_sum(void)
{
    dwi_sum.clear();
    dwi_sum.resize(voxel.dim);
    tipl::par_for(dwi_sum.size(),[&](unsigned int pos)
    {
        for (unsigned int index = 0;index < src_dwi_data.size();++index)
            dwi_sum[pos] += src_dwi_data[index][pos];
    });

    float max_value = *std::max_element(dwi_sum.begin(),dwi_sum.end());
    float min_value = max_value;
    for (unsigned int index = 0;index < dwi_sum.size();++index)
        if (dwi_sum[index] < min_value && dwi_sum[index] > 0)
            min_value = dwi_sum[index];


    tipl::minus_constant(dwi_sum,min_value);
    tipl::lower_threshold(dwi_sum,0.0f);
    float t = tipl::segmentation::otsu_threshold(dwi_sum);
    tipl::upper_threshold(dwi_sum,t*3.0f);
    tipl::normalize(dwi_sum,1.0);
}

void ImageModel::remove(unsigned int index)
{
    src_dwi_data.erase(src_dwi_data.begin()+index);
    src_bvalues.erase(src_bvalues.begin()+index);
    src_bvectors.erase(src_bvectors.begin()+index);
    shell.clear();
}

typedef boost::mpl::vector<
    ReadDWIData,
    Dwi2Tensor
> check_btable_process;
std::pair<float,float> evaluate_fib(
        const tipl::geometry<3>& dim,
        const std::vector<std::vector<float> >& fib_fa,
        const std::vector<std::vector<float> >& fib_dir)
{
    unsigned char num_fib = fib_fa.size();
    char dx[13] = {1,0,0,1,1,0, 1, 1, 0, 1,-1, 1, 1};
    char dy[13] = {0,1,0,1,0,1,-1, 0, 1, 1, 1,-1, 1};
    char dz[13] = {0,0,1,0,1,1, 0,-1,-1, 1, 1, 1,-1};
    std::vector<tipl::vector<3> > dis(13);
    for(unsigned int i = 0;i < 13;++i)
    {
        dis[i] = tipl::vector<3>(dx[i],dy[i],dz[i]);
        dis[i].normalize();
    }
    float otsu = *std::max_element(fib_fa[0].begin(),fib_fa[0].end())*0.1;
    std::vector<std::vector<unsigned char> > connected(fib_fa.size());
    for(unsigned int index = 0;index < connected.size();++index)
        connected[index].resize(dim.size());
    float connection_count = 0;
    for(tipl::pixel_index<3> index(dim);index < dim.size();++index)
    {
        if(fib_fa[0][index.index()] <= otsu)
            continue;
        unsigned int index3 = index.index()+index.index()+index.index();
        for(unsigned char fib1 = 0;fib1 < num_fib;++fib1)
        {
            if(fib_fa[fib1][index.index()] <= otsu)
                break;
            for(unsigned int j = 0;j < 2;++j)
            for(unsigned int i = 0;i < 13;++i)
            {
                tipl::vector<3,int> pos;
                pos = j ? tipl::vector<3,int>(index[0] + dx[i],index[1] + dy[i],index[2] + dz[i])
                          :tipl::vector<3,int>(index[0] - dx[i],index[1] - dy[i],index[2] - dz[i]);
                if(!dim.is_valid(pos))
                    continue;
                tipl::pixel_index<3> other_index(pos[0],pos[1],pos[2],dim);
                unsigned int other_index3 = other_index.index()+other_index.index()+other_index.index();
                if(std::abs(tipl::vector<3>(&fib_dir[fib1][index3])*dis[i]) <= 0.8665)
                    continue;
                for(unsigned char fib2 = 0;fib2 < num_fib;++fib2)
                    if(fib_fa[fib2][other_index.index()] > otsu &&
                            std::abs(tipl::vector<3>(&fib_dir[fib2][other_index3])*dis[i]) > 0.8665)
                    {
                        connected[fib1][index.index()] = 1;
                        connected[fib2][other_index.index()] = 1;
                        connection_count += fib_fa[fib2][other_index.index()];
                    }
            }
        }
    }
    float no_connection_count = 0;
    for(tipl::pixel_index<3> index(dim);index < dim.size();++index)
    {
        for(unsigned int i = 0;i < num_fib;++i)
            if(fib_fa[i][index.index()] > otsu && !connected[i][index.index()])
            {
                no_connection_count += fib_fa[i][index.index()];
            }

    }

    return std::make_pair(connection_count,no_connection_count);
}
void flip_fib_dir(std::vector<float>& fib_dir,const unsigned char* order)
{
    for(unsigned int j = 0;j+2 < fib_dir.size();j += 3)
    {
        float x = fib_dir[j+order[0]];
        float y = fib_dir[j+order[1]];
        float z = fib_dir[j+order[2]];
        fib_dir[j] = x;
        fib_dir[j+1] = y;
        fib_dir[j+2] = z;
        if(order[3])
            fib_dir[j] = -fib_dir[j];
        if(order[4])
            fib_dir[j+1] = -fib_dir[j+1];
        if(order[5])
            fib_dir[j+2] = -fib_dir[j+2];
    }
}
std::string ImageModel::check_b_table(void)
{
    set_title("checking b-table");
    bool output_dif = voxel.output_diffusivity;
    bool output_tensor = voxel.output_tensor;
    voxel.output_diffusivity = true;
    voxel.output_tensor = false;
    reconstruct<check_btable_process>();
    voxel.output_diffusivity = output_dif;
    voxel.output_tensor = output_tensor;
    std::vector<std::vector<float> > fib_fa(1);
    std::vector<std::vector<float> > fib_dir(1);
    fib_fa[0].swap(voxel.fib_fa);
    fib_dir[0].swap(voxel.fib_dir);

    const unsigned char order[18][6] = {
                            {0,1,2,1,0,0},
                            {0,1,2,0,1,0},
                            {0,1,2,0,0,1},
                            {0,2,1,1,0,0},
                            {0,2,1,0,1,0},
                            {0,2,1,0,0,1},
                            {1,0,2,1,0,0},
                            {1,0,2,0,1,0},
                            {1,0,2,0,0,1},
                            {1,2,0,1,0,0},
                            {1,2,0,0,1,0},
                            {1,2,0,0,0,1},
                            {2,1,0,1,0,0},
                            {2,1,0,0,1,0},
                            {2,1,0,0,0,1},
                            {2,0,1,1,0,0},
                            {2,0,1,0,1,0},
                            {2,0,1,0,0,1}};
    const char txt[18][7] = {".012fx",".012fy",".012fz",
                             ".021fx",".021fy",".021fz",
                             ".102fx",".102fy",".102fz",
                             ".120fx",".120fy",".120fz",
                             ".210fx",".210fy",".210fz",
                             ".201fx",".201fy",".201fz"};

    float result[18] = {0};
    float cur_score = evaluate_fib(voxel.dim,fib_fa,fib_dir).first;
    for(int i = 0;i < 18;++i)
    {
        std::vector<std::vector<float> > new_dir(fib_dir);
        flip_fib_dir(new_dir[0],order[i]);
        result[i] = evaluate_fib(voxel.dim,fib_fa,new_dir).first;
    }
    int best = std::max_element(result,result+18)-result;

    if(result[best] > cur_score)
    {
        std::cout << "b-table corrected." << std::endl;
        flip_b_table(order[best]);
        voxel.load_from_src(*this);
        return txt[best];
    }
    return std::string();
}

float ImageModel::quality_control_neighboring_dwi_corr(void)
{
    std::vector<std::pair<int,int> > corr_pairs;
    for(int i = 0;i < src_bvalues.size();++i)
    {
        if(src_bvalues[i] == 0.0f)
            continue;
        float min_dis = std::numeric_limits<float>::max();
        int min_j = 0;
        for(int j = i+1;j < src_bvalues.size();++j)
        {
            tipl::vector<3> v1(src_bvectors[i]),v2(src_bvectors[j]);
            v1 *= std::sqrt(src_bvalues[i]);
            v2 *= std::sqrt(src_bvalues[j]);
            float dis = std::min<float>((v1-v2).length(),(v1+v2).length());
            if(dis < min_dis)
            {
                min_dis = dis;
                min_j = j;
            }
        }
        corr_pairs.push_back(std::make_pair(i,min_j));
    }
    float self_cor = 0.0f;
    unsigned int count = 0;
    tipl::par_for(corr_pairs.size(),[&](int index)
    {
        int i1 = corr_pairs[index].first;
        int i2 = corr_pairs[index].second;
        std::vector<float> I1,I2;
        I1.reserve(voxel.dim.size());
        I2.reserve(voxel.dim.size());
        for(int i = 0;i < voxel.dim.size();++i)
            if(voxel.mask[i])
            {
                I1.push_back(src_dwi_data[i1][i]);
                I2.push_back(src_dwi_data[i2][i]);
            }
        self_cor += tipl::correlation(I1.begin(),I1.end(),I2.begin());
        ++count;
    });
    self_cor/= (float)count;
    return self_cor;
}
bool ImageModel::is_human_data(void) const
{
    return voxel.dim[0]*voxel.vs[0] > 100 && voxel.dim[1]*voxel.vs[1] > 120 && voxel.dim[2]*voxel.vs[2] > 40;
}
void ImageModel::flip_b_table(const unsigned char* order)
{
    for(unsigned int index = 0;index < src_bvectors.size();++index)
    {
        float x = src_bvectors[index][order[0]];
        float y = src_bvectors[index][order[1]];
        float z = src_bvectors[index][order[2]];
        src_bvectors[index][0] = x;
        src_bvectors[index][1] = y;
        src_bvectors[index][2] = z;
        if(order[3])
            src_bvectors[index][0] = -src_bvectors[index][0];
        if(order[4])
            src_bvectors[index][1] = -src_bvectors[index][1];
        if(order[5])
            src_bvectors[index][2] = -src_bvectors[index][2];
    }
    voxel.grad_dev.clear();
}
void ImageModel::flip_b_table(unsigned char dim)
{
    for(unsigned int index = 0;index < src_bvectors.size();++index)
        src_bvectors[index][dim] = -src_bvectors[index][dim];
    if(!voxel.grad_dev.empty())
    {
        // <Flip*Gra_dev*b_table,ODF>
        // = <(Flip*Gra_dev*inv(Flip))*Flip*b_table,ODF>
        unsigned char nindex[3][4] = {{1,2,3,6},{1,3,5,7},{2,5,6,7}};
        for(unsigned int index = 0;index < 4;++index)
        {
            // 1  0  0         1  0  0
            //[0 -1  0] *Grad*[0 -1  0]
            // 0  0  1         0  0  1
            unsigned char pos = nindex[dim][index];
            for(unsigned int i = 0;i < voxel.dim.size();++i)
                voxel.grad_dev[pos][i] = -voxel.grad_dev[pos][i];
        }
    }
}
// 0:xy 1:yz 2: xz
void ImageModel::swap_b_table(unsigned char dim)
{
    std::swap(voxel.vs[dim],voxel.vs[(dim+1)%3]);
    for (unsigned int index = 0;index < src_bvectors.size();++index)
        std::swap(src_bvectors[index][dim],src_bvectors[index][(dim+1)%3]);
    if(!voxel.grad_dev.empty())
    {
        unsigned char swap1[3][6] = {{0,3,6,0,1,2},{1,4,7,3,4,5},{0,3,6,0,1,2}};
        unsigned char swap2[3][6] = {{1,4,7,3,4,5},{2,5,8,6,7,8},{2,5,8,6,7,8}};
        for(unsigned int index = 0;index < 6;++index)
        {
            unsigned char s1 = swap1[dim][index];
            unsigned char s2 = swap2[dim][index];
            for(unsigned int i = 0;i < voxel.dim.size();++i)
                std::swap(voxel.grad_dev[s1][i],voxel.grad_dev[s2][i]);
        }
    }
}

// 0: x  1: y  2: z
// 3: xy 4: yz 5: xz
void ImageModel::flip_dwi(unsigned char type)
{
    if(type < 3)
        flip_b_table(type);
    else
        swap_b_table(type-3);
    tipl::flip(dwi_sum,type);
    tipl::flip(voxel.mask,type);
    for(unsigned int i = 0;i < voxel.grad_dev.size();++i)
    {
        auto I = tipl::make_image((float*)&*(voxel.grad_dev[i].begin()),voxel.dim);
        tipl::flip(I,type);
    }
    for (unsigned int index = 0;check_prog(index,src_dwi_data.size());++index)
    {
        auto I = tipl::make_image((unsigned short*)src_dwi_data[index],voxel.dim);
        tipl::flip(I,type);
    }
    voxel.dim = dwi_sum.geometry();
}
// used in eddy correction for each dwi
void ImageModel::rotate_one_dwi(unsigned int dwi_index,const tipl::transformation_matrix<double>& affine)
{
    tipl::image<float,3> tmp(voxel.dim);
    auto I = tipl::make_image((unsigned short*)src_dwi_data[dwi_index],voxel.dim);
    tipl::resample(I,tmp,affine,tipl::cubic);
    tipl::lower_threshold(tmp,0);
    std::copy(tmp.begin(),tmp.end(),I.begin());
    // rotate b-table
    tipl::matrix<3,3,float> iT = tipl::inverse(affine.get());
    tipl::vector<3> v;
    tipl::vector_rotation(src_bvectors[dwi_index].begin(),v.begin(),iT,tipl::vdim<3>());
    v.normalize();
    src_bvectors[dwi_index] = v;
}

void ImageModel::rotate(const tipl::image<float,3>& ref,
                        const tipl::transformation_matrix<double>& affine,
                        bool super_resolution)
{
    tipl::geometry<3> new_geo = ref.geometry();
    std::vector<tipl::image<unsigned short,3> > dwi(src_dwi_data.size());
    tipl::par_for2(src_dwi_data.size(),[&](unsigned int index,unsigned int id)
    {
        if(!id)
            check_prog(index,src_dwi_data.size());
        dwi[index].resize(new_geo);
        auto I = tipl::make_image((unsigned short*)src_dwi_data[index],voxel.dim);
        if(super_resolution)
            tipl::resample_with_ref(I,ref,dwi[index],affine);
        else
            tipl::resample(I,dwi[index],affine,tipl::cubic);
        src_dwi_data[index] = &(dwi[index][0]);
    });
    check_prog(0,0);
    dwi.swap(new_dwi);

    // rotate b-table
    if(has_image_rotation)
    {
        tipl::matrix<3,3,float> T = tipl::inverse(affine.get());
        src_bvectors_rotate *= T;
    }
    else
        src_bvectors_rotate = tipl::inverse(affine.get());
    has_image_rotation = true;


    if(!voxel.grad_dev.empty())
    {
        // <R*Gra_dev*b_table,ODF>
        // = <(R*Gra_dev*inv(R))*R*b_table,ODF>
        float det = std::abs(src_bvectors_rotate.det());
        begin_prog("rotating grad_dev");
        for(unsigned int index = 0;check_prog(index,voxel.dim.size());++index)
        {
            tipl::matrix<3,3,float> grad_dev,G_invR;
            for(unsigned int i = 0; i < 9; ++i)
                grad_dev[i] = voxel.grad_dev[i][index];
            G_invR = grad_dev*affine.get();
            grad_dev = src_bvectors_rotate*G_invR;
            for(unsigned int i = 0; i < 9; ++i)
                voxel.grad_dev[i][index] = grad_dev[i]/det;
        }
        std::vector<tipl::image<float,3> > new_gra_dev(voxel.grad_dev.size());
        begin_prog("rotating grad_dev volume");
        for (unsigned int index = 0;check_prog(index,new_gra_dev.size());++index)
        {
            new_gra_dev[index].resize(new_geo);
            tipl::resample(voxel.grad_dev[index],new_gra_dev[index],affine,tipl::cubic);
            voxel.grad_dev[index] = tipl::make_image((float*)&(new_gra_dev[index][0]),voxel.dim);
        }
        new_gra_dev.swap(voxel.new_grad_dev);
    }
    voxel.dim = new_geo;
    calculate_dwi_sum();
    voxel.calculate_mask(dwi_sum);

}
void ImageModel::trim(void)
{
    tipl::geometry<3> range_min,range_max;
    tipl::bounding_box(voxel.mask,range_min,range_max,0);
    for (unsigned int index = 0;check_prog(index,src_dwi_data.size());++index)
    {
        auto I = tipl::make_image((unsigned short*)src_dwi_data[index],voxel.dim);
        tipl::image<unsigned short,3> I0 = I;
        tipl::crop(I0,range_min,range_max);
        std::fill(I.begin(),I.end(),0);
        std::copy(I0.begin(),I0.end(),I.begin());
    }
    tipl::crop(voxel.mask,range_min,range_max);
    voxel.dim = voxel.mask.geometry();
    calculate_dwi_sum();
    voxel.calculate_mask(dwi_sum);
}

float interpo_pos(float v1,float v2,float u1,float u2)
{
    float w = (u2-u1-v2+v1);
    return std::max<float>(0.0,std::min<float>(1.0,w == 0.0f? 0:(v1-u1)/w));
}

template<typename image_type>
void get_distortion_map(const image_type& v1,
                        const image_type& v2,
                        tipl::image<float,3>& dis_map)
{
    int h = v1.height(),w = v1.width(),hw = v1.plane_size();
    dis_map.resize(v1.geometry());
    tipl::par_for(v1.depth(),[&](int z)
    {
    for(int x = 0;x < w;++x)
    {
        //int x = 46; for 2017_11_09 DSI-STS1
        //int z = 32;
        std::vector<float> cdf_y1(h),cdf_y2(h);//,cdf(h);
        for(int y = 0,pos = x + z*hw;y < h;++y,pos += w)
        {
            cdf_y1[y] =  (y ? v1[pos]+cdf_y1[y-1]:0);
            cdf_y2[y] =  (y ? v2[pos]+cdf_y2[y-1]:0);
        }
        //if(cdf_y1.back() == 0.0 || cdf_y2.back() == 0.0)
        //    continue;
        tipl::multiply_constant(cdf_y2,cdf_y1.back()/cdf_y2.back());

        for(int y = 0,pos = x + z*hw;y < h;++y,pos += w)
        {
            if(cdf_y1[y] == cdf_y2[y])
            {
                //cdf[y] = cdf_y1[y];
                continue;
            }
            int d = 1,y1,y2;
            float v1,v2,u1,u2;
            v2 = cdf_y1[y];
            u2 = cdf_y2[y];
            bool positive_d = true;
            if(cdf_y1[y] > cdf_y2[y])
            {
                for(;d < h;++d)
                {
                    y1 = y-d;
                    y2 = y+d;
                    v1 = v2;
                    u1 = u2;
                    v2 = (y1 >=0 ? cdf_y1[y1]:0);
                    u2 = (y2 < cdf_y2.size() ? cdf_y2[y2]:cdf_y2.back());
                    if(v2 <= u2)
                        break;
                }
            }
            else
            {
                for(;d < h;++d)
                {
                    y2 = y-d;
                    y1 = y+d;
                    v1 = v2;
                    u1 = u2;
                    v2 = (y1 < cdf_y1.size() ? cdf_y1[y1]:cdf_y1.back());
                    u2 = (y2 >= 0 ? cdf_y2[y2]:0);
                    if(v2 >= u2)
                        break;
                }
                positive_d = false;
            }
            //cdf[y] = v1+(v2-v1)*interpo_pos(v1,v2,u1,u2);
            dis_map[pos] = interpo_pos(v1,v2,u1,u2)+d-1;
            if(!positive_d)
                dis_map[pos] = -dis_map[pos];
        }

        /*
        std::cout << "A=[";
        for(int y = 0;y < h;++y)
            std::cout << cdf_y1[y] << " ";
        std::cout << "];" << std::endl;
        std::cout << "B=[";
        for(int y = 0;y < h;++y)
            std::cout << cdf_y2[y] << " ";
        std::cout << "];" << std::endl;
        std::cout << "C=[";
        for(int y = 0;y < h;++y)
            std::cout << cdf[y] << " ";
        std::cout << "];" << std::endl;
        std::cout << "D=[";
        for(int y = 0,pos = x + z*hw;y < h;++y,pos += h)
            std::cout << dis_map[pos] << " ";
        std::cout << "];" << std::endl;
        */
        //for(int y = 0,pos = x + z*geo.plane_size();y < h;++y,pos += geo.width())
        //    v1[pos] = std::max<float>(0.0f,cdf[y] - (y? cdf[y-1]:0));
    }
    }
    );
}

template<typename image_type,typename out_type>
void apply_distortion_map(const image_type& v1,
                          const image_type& v2,
                          const tipl::image<float,3>& dis_map,
                          out_type& dwi)
{
    int h = v1.height(),w = v1.width(),hw = v1.plane_size();
    dwi.resize(v1.geometry());
    tipl::par_for(v1.depth(),[&](int z)
    {
    for(int x = 0;x < w;++x)
    {
        std::vector<float> cdf_y1(h),cdf_y2(h),cdf(h);
        for(int y = 0,pos = x + z*hw;y < h;++y,pos += w)
        {
            cdf_y1[y] = v1[pos] + (y ? cdf_y1[y-1]:0);
            cdf_y2[y] = v2[pos] + (y ? cdf_y2[y-1]:0);
        }

        auto I1 = tipl::make_image(&cdf_y1[0],tipl::geometry<1>(cdf_y1.size()));
        auto I2 = tipl::make_image(&cdf_y2[0],tipl::geometry<1>(cdf_y2.size()));
        for(int y = 0,pos = x + z*hw;y < h;++y,pos += w)
        {
            float d = dis_map[pos];
            float y1 = y-d;
            float y2 = y+d;
            cdf[y] = tipl::estimate(I1,y1)+tipl::estimate(I2,y2);
            cdf[y] *= 0.5;
        }
        for(int y = 1,pos = x + z*hw+w;y < h;++y,pos += w)
            dwi[pos] = std::max<float>(0.0f,cdf[y] - (y? cdf[y-1]:0));
    }
    }
    );
}




void ImageModel::distortion_correction(const ImageModel& rhs)
{
    tipl::image<float,3> v1,v2;
    v1 = dwi_sum;
    v2 = rhs.dwi_sum;


    bool swap_xy = false;
    {
        tipl::image<float,2> px1,px2,py1,py2;
        tipl::project_x(v1,px1);
        tipl::project_x(v2,px2);
        tipl::project_y(v1,py1);
        tipl::project_y(v2,py2);
        float cx = tipl::correlation(px1.begin(),px1.end(),px2.begin());
        float cy = tipl::correlation(py1.begin(),py1.end(),py2.begin());

        if(cx > cy)
        {
            tipl::swap_xy(v1);
            tipl::swap_xy(v2);
            swap_xy = true;
        }
    }
    tipl::image<float,3> dis_map;
    tipl::filter::gaussian(v1);
    tipl::filter::gaussian(v2);
    get_distortion_map(v1,v2,dis_map);
    //dwi_sum = dis_map;
    //return;


    std::vector<tipl::image<unsigned short,3> > dwi(src_dwi_data.size());
    for(int i = 0;i < src_dwi_data.size();++i)
    {
        v1 = tipl::make_image(src_dwi_data[i],voxel.dim);
        v2 = tipl::make_image(rhs.src_dwi_data[i],rhs.voxel.dim);
        if(swap_xy)
        {
            tipl::swap_xy(v1);
            tipl::swap_xy(v2);
        }
        apply_distortion_map(v1,v2,dis_map,dwi[i]);
        if(swap_xy)
            tipl::swap_xy(dwi[i]);
    }


    new_dwi.swap(dwi);
    for(int i = 0;i < new_dwi.size();++i)
        src_dwi_data[i] = &(new_dwi[i][0]);

    calculate_dwi_sum();
    voxel.calculate_mask(dwi_sum);

}


void calculate_shell(const std::vector<float>& sorted_bvalues,
                     std::vector<unsigned int>& shell)
{
    if(sorted_bvalues.front() != 0.0f)
        shell.push_back(0);
    else
    {
        for(int i = 1;i < sorted_bvalues.size();++i)
            if(sorted_bvalues[i] != 0)
            {
                shell.push_back(i);
                break;
            }
    }
    for(unsigned int index = shell.back()+1;index < sorted_bvalues.size();++index)
        if(std::abs(sorted_bvalues[index]-sorted_bvalues[index-1]) > 100)
            shell.push_back(index);
}

void ImageModel::calculate_shell(void)
{
    std::vector<float> sorted_bvalues(src_bvalues);
    std::sort(sorted_bvalues.begin(),sorted_bvalues.end());
    ::calculate_shell(sorted_bvalues,shell);
}
bool ImageModel::is_dsi_half_sphere(void)
{
    if(shell.empty())
        calculate_shell();
    return is_dsi() && (shell[1] - shell[0] <= 3);
}

bool ImageModel::is_dsi(void)
{
    if(shell.empty())
        calculate_shell();
    return shell.size() > 4 && (shell[1] - shell[0] <= 6);
}
bool ImageModel::need_scheme_balance(void)
{
    if(shell.empty())
        calculate_shell();
    if(is_dsi() || shell.size() > 6)
        return false;
    for(int i = 0;i < shell.size();++i)
    {
        unsigned int from = shell[i];
        unsigned int to = (i + 1 == shell.size() ? shell.size():shell[i+1]);
        if(to-from < 128)
            return true;
    }
    return false;
}

bool ImageModel::is_multishell(void)
{
    if(shell.empty())
        calculate_shell();
    return (shell.size() > 1) && !is_dsi();
}


void ImageModel::get_report(std::string& report)
{
    std::vector<float> sorted_bvalues(src_bvalues);
    std::sort(sorted_bvalues.begin(),sorted_bvalues.end());
    unsigned int num_dir = 0;
    for(int i = 0;i < src_bvalues.size();++i)
        if(src_bvalues[i] > 50)
            ++num_dir;
    std::ostringstream out;
    if(is_dsi())
    {
        out << " A diffusion spectrum imaging scheme was used, and a total of " << num_dir
            << " diffusion sampling were acquired."
            << " The maximum b-value was " << (int)std::round(src_bvalues.back()) << " s/mm2.";
    }
    else
    if(is_multishell())
    {
        out << " A multishell diffusion scheme was used, and the b-values were ";
        for(unsigned int index = 0;index < shell.size();++index)
        {
            if(index > 0)
            {
                if(index == shell.size()-1)
                    out << " and ";
                else
                    out << " ,";
            }
            out << (int)std::round(sorted_bvalues[
                index == shell.size()-1 ? (sorted_bvalues.size()+shell.back())/2 : (shell[index+1] + shell[index])/2]);
        }
        out << " s/mm2.";

        out << " The number of diffusion sampling directions were ";
        for(unsigned int index = 0;index < shell.size()-1;++index)
            out << shell[index+1] - shell[index] << (shell.size() == 2 ? " ":", ");
        out << "and " << sorted_bvalues.size()-shell.back() << ", respectively.";
    }
    else
        if(shell.size() == 1)
        {
            if(num_dir < 100)
                out << " A DTI diffusion scheme was used, and a total of ";
            else
                out << " A HARDI scheme was used, and a total of ";
            out << num_dir
                << " diffusion sampling directions were acquired."
                << " The b-value was " << sorted_bvalues.back() << " s/mm2.";
        }

    out << " The in-plane resolution was " << voxel.vs[0] << " mm."
        << " The slice thickness was " << voxel.vs[2] << " mm.";
    report = out.str();
}

bool ImageModel::load_from_file(const char* dwi_file_name)
{
    file_name = dwi_file_name;
    if (!mat_reader.load_from_file(dwi_file_name))
    {
        error_msg = "Cannot open file";
        return false;
    }
    unsigned int row,col;

    const unsigned short* dim_ptr = 0;
    if (!mat_reader.read("dimension",row,col,dim_ptr))
    {
        error_msg = "Cannot find dimension matrix";
        return false;
    }
    const float* voxel_size = 0;
    if (!mat_reader.read("voxel_size",row,col,voxel_size))
    {
        //error_msg = "Cannot find voxel size matrix";
        //return false;
        std::fill(voxel.vs.begin(),voxel.vs.end(),2.0);
    }
    else
        std::copy(voxel_size,voxel_size+3,voxel.vs.begin());

    if (dim_ptr[0]*dim_ptr[1]*dim_ptr[2] <= 0)
    {
        error_msg = "Invalid dimension setting";
        return false;
    }
    voxel.dim[0] = dim_ptr[0];
    voxel.dim[1] = dim_ptr[1];
    voxel.dim[2] = dim_ptr[2];

    const float* table;
    if (!mat_reader.read("b_table",row,col,table))
    {
        error_msg = "Cannot find b_table matrix";
        return false;
    }
    src_bvalues.resize(col);
    src_bvectors.resize(col);
    for (unsigned int index = 0;index < col;++index)
    {
        src_bvalues[index] = table[0];
        src_bvectors[index][0] = table[1];
        src_bvectors[index][1] = table[2];
        src_bvectors[index][2] = table[3];
        src_bvectors[index].normalize();
        table += 4;
    }

    const char* report_buf = 0;
    if(mat_reader.read("report",row,col,report_buf))
        voxel.report = std::string(report_buf,report_buf+row*col);
    else
        get_report(voxel.report);

    src_dwi_data.resize(src_bvalues.size());
    for (unsigned int index = 0;index < src_bvalues.size();++index)
    {
        std::ostringstream out;
        out << "image" << index;
        mat_reader.read(out.str().c_str(),row,col,src_dwi_data[index]);
        if (!src_dwi_data[index])
        {
            error_msg = "Cannot find image matrix";
            return false;
        }
    }


    {
        const float* grad_dev = 0;
        if(mat_reader.read("grad_dev",row,col,grad_dev) && row*col == voxel.dim.size()*9)
        {
            for(unsigned int index = 0;index < 9;index++)
                voxel.grad_dev.push_back(tipl::make_image((float*)grad_dev+index*voxel.dim.size(),voxel.dim));
            if(std::fabs(voxel.grad_dev[0][0])+std::fabs(voxel.grad_dev[4][0])+std::fabs(voxel.grad_dev[8][0]) < 1.0)
            {
                tipl::add_constant(voxel.grad_dev[0].begin(),voxel.grad_dev[0].end(),1.0);
                tipl::add_constant(voxel.grad_dev[4].begin(),voxel.grad_dev[4].end(),1.0);
                tipl::add_constant(voxel.grad_dev[8].begin(),voxel.grad_dev[8].end(),1.0);
            }
        }

    }

    // create mask;
    calculate_dwi_sum();

    const unsigned char* mask_ptr = 0;
    if(mat_reader.read("mask",row,col,mask_ptr))
    {
        voxel.mask.resize(voxel.dim);
        if(row*col == voxel.dim.size())
            std::copy(mask_ptr,mask_ptr+row*col,voxel.mask.begin());
    }
    else
        voxel.calculate_mask(dwi_sum);
    return true;
}

void ImageModel::save_to_file(gz_mat_write& mat_writer)
{

    set_title("saving");

    // dimension
    {
        short dim[3];
        dim[0] = voxel.dim[0];
        dim[1] = voxel.dim[1];
        dim[2] = voxel.dim[2];
        mat_writer.write("dimension",dim,1,3);
    }

    // voxel size
    mat_writer.write("voxel_size",&*voxel.vs.begin(),1,3);

    std::vector<float> float_data;
    std::vector<short> short_data;
    voxel.ti.save_to_buffer(float_data,short_data);
    mat_writer.write("odf_vertices",&*float_data.begin(),3,voxel.ti.vertices_count);
    mat_writer.write("odf_faces",&*short_data.begin(),3,voxel.ti.faces.size());

}
void ImageModel::save_fib(const std::string& ext)
{
    std::string output_name = file_name;
    output_name += ext;
    begin_prog("saving data");
    gz_mat_write mat_writer(output_name.c_str());
    save_to_file(mat_writer);
    voxel.end(mat_writer);
    std::string final_report = voxel.report.c_str();
    final_report += voxel.recon_report.str();
    mat_writer.write("report",final_report.c_str(),1,final_report.length());
}
bool ImageModel::save_to_nii(const char* nifti_file_name) const
{
    gz_nifti header;
    float vs[4];
    std::copy(voxel.vs.begin(),voxel.vs.end(),vs);
    vs[3] = 1.0;
    header.set_voxel_size(vs);
    tipl::geometry<4> nifti_dim;
    std::copy(voxel.dim.begin(),voxel.dim.end(),nifti_dim.begin());
    nifti_dim[3] = src_bvalues.size();
    tipl::image<unsigned short,4> buffer(nifti_dim);
    for(unsigned int index = 0;index < src_bvalues.size();++index)
    {
        std::copy(src_dwi_data[index],
                  src_dwi_data[index]+voxel.dim.size(),
                  buffer.begin() + (size_t)index*voxel.dim.size());
    }
    tipl::flip_xy(buffer);
    header << buffer;
    return header.save_to_file(nifti_file_name);
}
bool ImageModel::save_b0_to_nii(const char* nifti_file_name) const
{
    gz_nifti header;
    header.set_voxel_size(voxel.vs);
    tipl::image<unsigned short,3> buffer(src_dwi_data[0],voxel.dim);
    tipl::flip_xy(buffer);
    header << buffer;
    return header.save_to_file(nifti_file_name);
}
bool ImageModel::save_b_table(const char* file_name) const
{
    std::ofstream out(file_name);
    for(unsigned int index = 0;index < src_bvalues.size();++index)
    {
        out << src_bvalues[index] << " "
            << src_bvectors[index][0] << " "
            << src_bvectors[index][1] << " "
            << src_bvectors[index][2] << std::endl;
    }
    return out.good();
}
bool ImageModel::save_bval(const char* file_name) const
{
    std::ofstream out(file_name);
    for(unsigned int index = 0;index < src_bvalues.size();++index)
    {
        if(index)
            out << " ";
        out << src_bvalues[index];
    }
    return out.good();
}
bool ImageModel::save_bvec(const char* file_name) const
{
    std::ofstream out(file_name);
    for(unsigned int index = 0;index < src_bvalues.size();++index)
    {
        out << src_bvectors[index][0] << " "
            << src_bvectors[index][1] << " "
            << src_bvectors[index][2] << std::endl;
    }
    return out.good();
}
