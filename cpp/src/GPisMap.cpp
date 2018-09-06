/*
 * GPisMap - Online Continuous Mapping using Gaussian Process Implicit Surfaces
 * https://github.com/leebhoram/GPisMap
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License v3 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of any FITNESS FOR A
 * PARTICULAR PURPOSE. See the GNU General Public License v3 for more details.
 *
 * You should have received a copy of the GNU General Public License v3
 * along with this program; if not, you can access it online at
 * http://www.gnu.org/licenses/gpl-3.0.html.
 *
 * Authors: Bhoram Lee <bhoram.lee@gmail.com>
 */
#include "GPisMap.h"
#include <algorithm>
#include <thread>

tree_param QuadTree::param = tree_param(0.2,102.4,12.8, 0.8); // the parameters must be (some common base)^n (n: integer)

static std::chrono::high_resolution_clock::time_point t1;
static std::chrono::high_resolution_clock::time_point t2;

#define MAX_RANGE 3e1
#define MIN_RANGE 2e-1

static inline bool isRangeValid(FLOAT r)
{
    return (r < MAX_RANGE) &&  (r > MIN_RANGE);
}

static inline FLOAT occ_test(FLOAT rinv, FLOAT rinv0, FLOAT a)
{
    return 2.0*(1.0/(1.0+exp(-a*(rinv-rinv0)))-0.5);
}

static inline FLOAT saturate(FLOAT val, FLOAT min_val, FLOAT max_val)
{
    return std::min(std::max(val,min_val),max_val);
}

static void polar2Cart(FLOAT a, FLOAT r, FLOAT& x, FLOAT& y)
{
    x = r*cos(a);
    y = r*sin(a);
    return;
}
static void cart2polar(FLOAT x, FLOAT y, FLOAT& a, FLOAT &r)
{
    a = atan2(y,x);
    r = sqrt(x*x + y*y);
    return;
}
// #endif

GPisMap::GPisMap():t(0),
                   gpo(0),
                   obs_numdata(0)
{
    init();
}

GPisMap::GPisMap(GPisMapParam par):t(0),
                                   gpo(0),
                                   obs_numdata(0),
                                   setting(par)
{
    init();
}

GPisMap::~GPisMap()
{
    reset();
}

void GPisMap::init(){
    pose_tr.resize(2);
    pose_R.resize(4);
}

void GPisMap::reset(){
    if (t!=0){
        delete t;
        t = 0;
    }

    if (gpo!=0){
        delete gpo;
        gpo = 0;
    }

    obs_numdata = 0;

    activeSet.clear();

    return;
}

bool GPisMap::preproData( FLOAT * datax,  FLOAT * dataf, int N, std::vector<FLOAT> & pose)
{
    if (datax == 0 || dataf == 0 || N < 1)
        return false;

    obs_theta.clear();
    obs_range.clear();
    obs_f.clear();
    obs_xylocal.clear();
    obs_xyglobal.clear();
    range_obs_max= 0.0;


    if (pose.size() != 6)
        return false;


    std::copy(pose.begin(),pose.begin()+2,pose_tr.begin());
    std::copy(pose.begin()+2,pose.end(),pose_R.begin());

    obs_numdata = 0;
    for (int k=0; k<N; k++)
    {
        FLOAT xloc = 0.0;
        FLOAT yloc = 0.0;
        if ( isRangeValid(dataf[k]) ){
            if (range_obs_max < dataf[k])
                range_obs_max = dataf[k];
            obs_theta.push_back(datax[k]);
            obs_range.push_back(dataf[k]);
            obs_f.push_back(1.0/std::sqrt(dataf[k]));
            polar2Cart(datax[k] , dataf[k], xloc, yloc);
            obs_xylocal.push_back(xloc);
            obs_xylocal.push_back(yloc);
            xloc += setting.sensor_offset[0];
            yloc += setting.sensor_offset[1];
            obs_xyglobal.push_back(pose_R[0]*xloc + pose_R[2]*yloc + pose_tr[0]); // indices based on the matlab convention
            obs_xyglobal.push_back(pose_R[1]*xloc + pose_R[3]*yloc + pose_tr[1]);
            obs_numdata++;
        }
    }

    if (obs_numdata > 1)
        return true;

    return false;
}

void GPisMap::update( FLOAT * datax,  FLOAT * dataf, int N, std::vector<FLOAT> & pose)
{
    if (!preproData(datax,dataf,N,pose))
        return;

    // Step 1
    if (regressObs()){

        // Step 2
        updateMapPoints();
        // Step 3
        addNewMeas();
        // Step 4
        updateGPs();
    }
    return;
}

void GPisMap::update_mt( FLOAT * datax,  FLOAT * dataf, int N, std::vector<FLOAT> & pose)
{
    if (!preproData(datax,dataf,N,pose))
        return;

    // Step 1
    if (regressObs()){

        // Step 2
        updateMapPoints();
        // Step 3
        addNewMeas();
        // Step 4
        updateGPs_mt();
    }
    return;
}

bool GPisMap::regressObs( ){
    t1= std::chrono::high_resolution_clock::now();
    int N[2];
    if (gpo == 0){
        gpo = new ObsGP1D();
    }

    N[0] = obs_numdata;
    gpo->reset();
    gpo->train( obs_theta.data(),obs_f.data(), N);
    t2= std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> time_collapsed = std::chrono::duration_cast<std::chrono::duration<double>>(t2-t1); // reset
    runtime[0] = time_collapsed.count();
    return gpo->isTrained();
}

void GPisMap::updateMapPoints(){
    t1= std::chrono::high_resolution_clock::now();
    if (t!=0 && gpo !=0){

        AABB searchbb(pose_tr[0],pose_tr[1],range_obs_max);
        std::vector<QuadTree*> quads;
        t->QueryNonEmptyLevelC(searchbb,quads);

        if (quads.size() > 0){

            FLOAT r2 = range_obs_max*range_obs_max;
            int k=0;
            for (auto it = quads.begin(); it != quads.end(); it++, k++) {

                Point<FLOAT> ct = (*it)->getCenter();
                FLOAT l = (*it)->getHalfLength();
                FLOAT sqr_range = (ct.x-pose_tr[0])*(ct.x-pose_tr[0]) + (ct.y-pose_tr[1])*(ct.y-pose_tr[1]);

                if (sqr_range > (r2 + 2*l*l)){ // out_of_range
                    continue;
                }

                std::vector<Point<FLOAT> > ext;
                ext.push_back((*it)->getNW());
                ext.push_back((*it)->getNE());
                ext.push_back((*it)->getSW());
                ext.push_back((*it)->getSE());

                int within_angle = 0;
                for (auto it_ = ext.begin();it_ != ext.end(); it_++){
                    FLOAT x_loc = pose_R[0]*((*it_).x-pose_tr[0])+pose_R[1]*((*it_).y-pose_tr[1]);
                    FLOAT y_loc = pose_R[2]*((*it_).x-pose_tr[0])+pose_R[3]*((*it_).y-pose_tr[1]);
                    x_loc -= setting.sensor_offset[0];
                    y_loc -= setting.sensor_offset[1];
                    FLOAT ang = 0.0;
                    FLOAT r = 0.0;
                    cart2polar(x_loc,y_loc,ang,r);
                    within_angle += int(( ang > setting.angle_obs_limit[0]) && (ang < setting.angle_obs_limit[1]));
                }

                if (within_angle == 0){
                    continue;
                }

                // Get all the nodes
                std::vector<std::shared_ptr<Node> > nodes;
                (*it)->getAllChildrenNonEmptyNodes(nodes);

                reEvalPoints(nodes);
            }
        }
    }
    t2= std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> time_collapsed = std::chrono::duration_cast<std::chrono::duration<double>>(t2-t1); // reset
    runtime[1] = time_collapsed.count();
    return;
}


void GPisMap::reEvalPoints(std::vector<std::shared_ptr<Node> >& nodes){
    //t1= std::chrono::high_resolution_clock::now();
    // placeholders
    EMatrixX amx(1,1);
    EVectorX rinv0(1);
    EVectorX var(1);
    FLOAT ang = 0.0;
    FLOAT r = 0.0;

    // For each point
    for (auto it=nodes.begin(); it != nodes.end(); it++){

        Point<FLOAT> pos = (*it)->getPos();

        FLOAT x_loc = pose_R[0]*(pos.x-pose_tr[0])+pose_R[1]*(pos.y-pose_tr[1]);
        FLOAT y_loc = pose_R[2]*(pos.x-pose_tr[0])+pose_R[3]*(pos.y-pose_tr[1]);
        x_loc -= setting.sensor_offset[0];
        y_loc -= setting.sensor_offset[1];
        cart2polar(x_loc,y_loc,ang,r);

        amx(0) = ang;
        gpo->test(amx,rinv0,var);

        // If unobservable, continue
        if (var(0) > setting.obs_var_thre)
            continue;

        FLOAT oc = occ_test(1.0/std::sqrt(r), rinv0(0), r*30.0);

        // If unobservable, continue
        if (oc < -0.1) // TO-DO : set it as a parameter
            continue;


        // gradient in the local coord.
        Point<FLOAT> grad = (*it)->getGrad();
        FLOAT grad_loc[2];
        grad_loc[0] = pose_R[0]*grad.x + pose_R[1]*grad.y;
        grad_loc[1] = pose_R[2]*grad.x + pose_R[3]*grad.y;


        /// Compute a new position
        // Iteratively move along the normal direction.
        FLOAT abs_oc = fabs(oc);
        FLOAT dx = setting.delx;
        FLOAT x_new[2] = {x_loc, y_loc};
        FLOAT r_new = r;
        for (int i=0; i<10 && abs_oc > 0.02; i++){ // TO-DO : set it as a parameter
            // move one step
            // (the direction is determined by the occupancy sign,
            //  the step size is heuristically determined accordint to iteration.)
            if (oc < 0){
                x_new[0] += grad_loc[0]*dx;
                x_new[1] += grad_loc[1]*dx;
            }
            else{
                x_new[0] -= grad_loc[0]*dx;
                x_new[1] -= grad_loc[1]*dx;
            }

            // test the new point

            cart2polar(x_new[0],x_new[1],ang,r_new);
            amx(0) = ang;
            gpo->test(amx,rinv0,var);

            if (var(0) > setting.obs_var_thre)
                break;
            else{
                FLOAT oc_new = occ_test(1.0/std::sqrt(r_new), rinv0(0), r_new*30.0);
                FLOAT abs_oc_new = fabs(oc_new);

                if (abs_oc_new < 0.02 || oc < -0.1) // TO-DO : set it as a parameter
                    break;
                else if (oc*oc_new < 0.0)
                    dx = 0.5*dx; // TO-DO : set it as a parameter
                else
                    dx = 1.1*dx; // TO-DO : set it as a parameter

                abs_oc = abs_oc_new;
                oc = oc_new;
            }

        }

        // Compute its gradient and uncertainty
        FLOAT Xperturb[4] = {1.0, -1.0, 0.0, 0.0};
        FLOAT Yperturb[4] = {0.0, 0.0, 1.0, -1.0};
        FLOAT occ[4] = {-1.0,-1.0,-1.0,-1.0};
        FLOAT occ_mean = 0.0;
        FLOAT r0_mean = 0.0;
        FLOAT r0_sqr_sum = 0.0;
        for (int i=0; i<4; i++){
            Xperturb[i] = x_new[0] + setting.delx*Xperturb[i];
            Yperturb[i] = x_new[1] + setting.delx*Yperturb[i];

            FLOAT r_;
            cart2polar(Xperturb[i], Yperturb[i], ang, r_);
            amx(0,0) = ang;
            gpo->test(amx,rinv0,var);

            if (var(0) > setting.obs_var_thre)
            {
                break;
            }
            occ[i] = occ_test(1.0/std::sqrt(r_), rinv0(0), r_*30.0);
            occ_mean += 0.25*occ[i];
            FLOAT r0 = 1.0/(rinv0(0)*rinv0(0));
            r0_sqr_sum += r0*r0;
            r0_mean += 0.25*r0;
        }

        if (var(0) > setting.obs_var_thre)// invalid
            continue;

        Point<FLOAT> grad_new_loc,grad_new;
        FLOAT norm_grad_new = 0.0;

        grad_new_loc.x = (occ[0] -occ[1])/setting.delx;
        grad_new_loc.y = (occ[2] -occ[3])/setting.delx;
        norm_grad_new = std::sqrt(grad_new_loc.x*grad_new_loc.x + grad_new_loc.y*grad_new_loc.y);

        if (norm_grad_new <1e-3){ // uncertainty increased
            (*it)->updateNoise(2.0*(*it)->getPosNoise(),2.0*(*it)->getGradNoise());
            continue;
        }


        FLOAT r_var = r0_sqr_sum/3.0 - r0_mean*r0_mean*4.0/3.0;
        r_var /= setting.delx;
        FLOAT noise = 100.0;
        FLOAT grad_noise = 1.0;
        if (norm_grad_new > 1e-6){
            grad_new_loc.x = grad_new_loc.x/norm_grad_new;
            grad_new_loc.y = grad_new_loc.y/norm_grad_new;
            noise = setting.min_position_noise*saturate(r_new*r_new, 1.0, noise);
            grad_noise = saturate(std::fabs(occ_mean)+r_var,setting.min_grad_noise,grad_noise);
        }
        else{
            noise = setting.min_position_noise*noise;
        }


        FLOAT dist = std::sqrt(x_new[0]*x_new[0]+x_new[1]*x_new[1]);
        FLOAT view_ang = std::max(-(x_new[0]*grad_new_loc.x+x_new[1]*grad_new_loc.y)/dist, (FLOAT)1e-1);
        FLOAT view_ang2 = view_ang*view_ang;
        FLOAT view_noise = setting.min_position_noise*((1.0-view_ang2)/view_ang2);

        FLOAT temp = noise;
        noise += view_noise + abs_oc;
        grad_noise = grad_noise + 0.1*view_noise;


        // local to global coord.
        Point<FLOAT> pos_new;
        x_new[0] += setting.sensor_offset[0];
        x_new[1] += setting.sensor_offset[1];
        pos_new.x = pose_R[0]*x_new[0] + pose_R[2]*x_new[1] + pose_tr[0];
        pos_new.y = pose_R[1]*x_new[0] + pose_R[3]*x_new[1] + pose_tr[1];
        grad_new.x = pose_R[0]*grad_new_loc.x + pose_R[2]*grad_new_loc.y;
        grad_new.y = pose_R[1]*grad_new_loc.x + pose_R[3]*grad_new_loc.y;

        FLOAT noise_old = (*it)->getPosNoise();
        FLOAT grad_noise_old = (*it)->getGradNoise();

        FLOAT pos_noise_sum = (noise_old + noise);
        FLOAT grad_noise_sum = (grad_noise_old + grad_noise);

         // Now, update
        if (grad_noise_old > 0.5 || grad_noise_old > 0.6){
           ;
        }
        else{
            // Position update
            pos_new.x = (noise*pos.x + noise_old*pos_new.x)/pos_noise_sum;
            pos_new.y = (noise*pos.y + noise_old*pos_new.y)/pos_noise_sum;
            FLOAT dist = 0.5*std::sqrt((pos.x-pos_new.x)*(pos.x-pos_new.x) + (pos.y-pos_new.y)*(pos.y-pos_new.y));

            // Normal update
            Point<FLOAT> tempv;
            tempv.x =  grad.x*grad_new.x + grad.y*grad_new.y;
            tempv.y = -grad.y*grad_new.x + grad.x*grad_new.y;
            FLOAT ang_dist = atan2(tempv.y,tempv.x)*noise/pos_noise_sum;
            FLOAT sina = sin(ang_dist);
            FLOAT cosa = cos(ang_dist);
            grad_new.x = cosa*grad.x - sina*grad.y;
            grad_new.y = sina*grad.x + cosa*grad.y;

            // Noise update
            grad_noise = std::min((FLOAT)1.0, std::max(grad_noise*grad_noise_old/grad_noise_sum + dist, setting.map_noise_param));

            noise = std::max((noise*noise_old/pos_noise_sum + dist), setting.map_noise_param);
        }
        // remove
        t->Remove(*it,activeSet);

        if (noise > 1.0 && grad_noise > 0.61){
            continue;
        }

        else{
            // try inserting
            std::shared_ptr<Node> p(new Node(pos_new));
            std::unordered_set<QuadTree*> vecInserted;

            bool succeeded = false;
            if (!t->IsNotNew(p)){
                succeeded = t->Insert(p,vecInserted);
                if (succeeded){
                    if (t->IsRoot() == false){
                        t = t->getRoot();
                    }
                }
            }

            if ((succeeded == 0) || !(vecInserted.size() > 0)) // if failed, then continue to test the next point
                continue;

            // update the point
            p->updateData(-setting.fbias, noise, grad_new, grad_noise, NODE_TYPE::HIT);

            for (auto itv = vecInserted.begin(); itv != vecInserted.end(); itv++)
                activeSet.insert(*itv);
        }
    }
    //t2= std::chrono::high_resolution_clock::now();
    return;
}

void GPisMap::addNewMeas(){
    // create if not initialized
    if (t == 0){
        t = new QuadTree(Point<FLOAT>(0.0,0.0));
    }
    evalPoints();
    return;
}


void GPisMap::evalPoints(){

    t1= std::chrono::high_resolution_clock::now();
     if (t == 0 || obs_numdata < 1)
         return;

    // For each point
    for (int k=0; k<obs_numdata; k++){
        int k2 = 2*k;

        // placeholder;
        EVectorX rinv0(1);
        EVectorX var(1);
        EMatrixX amx(1,1);

        amx(0,0) = obs_theta[k];
        gpo->test(amx,rinv0,var);

        if (var(0) > setting.obs_var_thre)
        {
            continue;
        }

        /////////////////////////////////////////////////////////////////
        // Try inserting
        Point<FLOAT> pt(obs_xyglobal[k2],obs_xyglobal[k2+1]);

        std::shared_ptr<Node> p(new Node(pt));
        std::unordered_set<QuadTree*> vecInserted;

        bool succeeded = false;
        if (!t->IsNotNew(p)){
            succeeded = t->Insert(p,vecInserted);
            if (succeeded){
                if (t->IsRoot() == false){
                    t = t->getRoot();
                }
            }
        }


        if ((succeeded == 0) || !(vecInserted.size() > 0)) // if failed, then continue to test the next point
            continue;

        /////////////////////////////////////////////////////////////////
        // if succeeded, then compute surface normal and uncertainty
        FLOAT Xperturb[4] = {1.0, -1.0, 0.0, 0.0};
        FLOAT Yperturb[4] = {0.0, 0.0, 1.0, -1.0};
        FLOAT occ[4] = {-1.0,-1.0,-1.0,-1.0};
        FLOAT occ_mean = 0.0;
        int i=0;
        for (; i<4; i++){
            Xperturb[i] = obs_xylocal[k2] + setting.delx*Xperturb[i];
            Yperturb[i] = obs_xylocal[k2+1] + setting.delx*Yperturb[i];

            FLOAT a,r;
            cart2polar(Xperturb[i], Yperturb[i], a, r);

            amx(0,0) = a;
            gpo->test(amx,rinv0,var);

            if (var(0) > setting.obs_var_thre)
            {
                break;
            }
            occ[i] = occ_test(1.0/std::sqrt(r), rinv0(0), r*30.0);
            occ_mean += 0.25*occ[i];
        }

        if (var(0) > setting.obs_var_thre){
            t->Remove(p);
            continue;
        }


        FLOAT noise = 100.0;
        FLOAT grad_noise = 1.00;
        Point<FLOAT> grad;

        grad.x = (occ[0] -occ[1])/setting.delx;
        grad.y = (occ[2] -occ[3])/setting.delx;
        FLOAT norm_grad = grad.x*grad.x + grad.y*grad.y;
        if (norm_grad > 1e-6){
            norm_grad = std::sqrt(norm_grad);
            FLOAT grad_loc_x = grad.x/norm_grad;
            FLOAT grad_loc_y = grad.y/norm_grad;

            grad.x = pose_R[0]*grad_loc_x + pose_R[2]*grad_loc_y;
            grad.y = pose_R[1]*grad_loc_x + pose_R[3]*grad_loc_y;

            noise = setting.min_position_noise*(saturate(obs_range[k]*obs_range[k], 1.0, noise));
            grad_noise = saturate(std::fabs(occ_mean),setting.min_grad_noise,grad_noise);
            FLOAT dist = std::sqrt(obs_xylocal[k2]*obs_xylocal[k2]+obs_xylocal[k2+1]*obs_xylocal[k2+1]);
            FLOAT view_ang = std::max(-(obs_xylocal[k2]*grad_loc_x+obs_xylocal[k2+1]*grad_loc_y)/dist, (FLOAT)1e-1);
            FLOAT view_ang2 = view_ang*view_ang;
            FLOAT view_noise = setting.min_position_noise*((1.0-view_ang2)/view_ang2);
            noise += view_noise;
        }

        /////////////////////////////////////////////////////////////////
        // update the point
        p->updateData(-setting.fbias, noise, grad,grad_noise, NODE_TYPE::HIT);

        for (auto it = vecInserted.begin(); it != vecInserted.end(); it++){
            activeSet.insert(*it);
        }
    }

   // t->printNodes();
   //std::cout << t->getNodeCount() << " points are in the map" << std::endl;
   //std::cout << "Active set size: " << activeSet.size() << std::endl;
   //t->printBoundary();
    t2= std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> time_collapsed = std::chrono::duration_cast<std::chrono::duration<double>>(t2-t1); // reset
    runtime[2] = time_collapsed.count();
    return;
}

void GPisMap::updateGPs(){
    t1= std::chrono::high_resolution_clock::now();
    int k = 0;

    std::unordered_set<QuadTree*> updateSet(activeSet);
    for (auto it = activeSet.begin(); it!= activeSet.end(); it++){

        Point<FLOAT> ct = (*it)->getCenter();
        FLOAT l = (*it)->getHalfLength();
        AABB searchbb(ct.x,ct.y,2.0*l);
        std::vector<QuadTree*> qs;
        t->QueryNonEmptyLevelC(searchbb,qs);
        if (qs.size()>0){
            for (auto itq = qs.begin(); itq!=qs.end(); itq++){
                updateSet.insert(*itq);
            }
        }
    }

    for (auto it = updateSet.begin(); it!= updateSet.end(); it++){

        if ((*it) != 0){
            Point<FLOAT> ct = (*it)->getCenter();
            FLOAT l = (*it)->getHalfLength();
            AABB searchbb(ct.x,ct.y,l*2.0);
            std::vector<std::shared_ptr<Node> > res;
            res.clear();
            t->QueryRange(searchbb,res);
            if (res.size()>0){
                std::shared_ptr<OnGPIS> gp(new OnGPIS(setting.map_scale_param, setting.map_noise_param));
                gp->train(res);
                (*it)->Update(gp);
            }
        }
    }
    // clear active set once all the jobs for update are done.
    activeSet.clear();
    t2= std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> time_collapsed = std::chrono::duration_cast<std::chrono::duration<double>>(t2-t1); // reset
    runtime[3] = time_collapsed.count();
    return;
}

void GPisMap::updateGPs_kernel(int thread_idx,
                               int start_idx,
                               int end_idx,
                               QuadTree **nodes_to_update){
    std::vector<std::shared_ptr<Node> > res;
    for (int i = start_idx; i < end_idx; ++i){
        if (nodes_to_update[i] != 0){
            Point<FLOAT> ct = (nodes_to_update[i])->getCenter();
            FLOAT l = (nodes_to_update[i])->getHalfLength();
            AABB searchbb(ct.x,ct.y,l*2.0);
            res.clear();
            t->QueryRange(searchbb,res);
            if (res.size()>0){
                std::shared_ptr<OnGPIS> gp(new OnGPIS(setting.map_scale_param, setting.map_noise_param));
                gp->train(res);
                (nodes_to_update[i])->Update(gp);
            }
        }
    }

}

void GPisMap::updateGPs_mt(){
    t1= std::chrono::high_resolution_clock::now();

    std::unordered_set<QuadTree*> updateSet(activeSet);

    int num_threads = std::thread::hardware_concurrency();
    std::thread *threads = new std::thread[num_threads];

    int num_threads_to_use = num_threads;

    for (auto it = activeSet.begin(); it!= activeSet.end(); it++){

        Point<FLOAT> ct = (*it)->getCenter();
        FLOAT l = (*it)->getHalfLength();
        AABB searchbb(ct.x,ct.y,2.0*l);
        std::vector<QuadTree*> qs;
        t->QueryNonEmptyLevelC(searchbb,qs);
        if (qs.size()>0){
            for (auto itq = qs.begin(); itq!=qs.end(); itq++){
                updateSet.insert(*itq);
            }
        }
    }

    int num_elements = updateSet.size();
    QuadTree **nodes_to_update = new QuadTree*[num_elements];
    int it_counter = 0;
    for (auto it = updateSet.begin(); it != updateSet.end(); ++it, ++it_counter){
        nodes_to_update[it_counter] = *it;
    }

    if (num_elements < num_threads){
        num_threads_to_use = num_elements;
    }
    else{
        num_threads_to_use = num_threads;
    }
    int num_leftovers = num_elements % num_threads_to_use;
    int batch_size = num_elements / num_threads_to_use;
    int element_cursor = 0;
    for(int i = 0; i < num_leftovers; ++i){
        threads[i] = std::thread(&GPisMap::updateGPs_kernel,
                                 this,
                                 i,
                                 element_cursor,
                                 element_cursor + batch_size + 1,
                                 nodes_to_update);
        element_cursor += batch_size + 1;

    }
    for (int i = num_leftovers; i < num_threads_to_use; ++i){
        threads[i] = std::thread(&GPisMap::updateGPs_kernel,
                                 this,
                                 i,
                                 element_cursor,
                                 element_cursor + batch_size,
                                 nodes_to_update);
        element_cursor += batch_size;
    }

    for (int i = 0; i < num_threads_to_use; ++i){
        threads[i].join();
    }

    delete [] nodes_to_update;
    delete [] threads;
    // clear active set once all the jobs for update are done.
    activeSet.clear();
    t2= std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> time_collapsed = std::chrono::duration_cast<std::chrono::duration<double>>(t2-t1); // reset
    runtime[3] = time_collapsed.count();
    return;
}

bool GPisMap::test(  FLOAT * x, int dim, int leng, FLOAT * res){
    if (x == 0 || dim != mapDimension || leng < 1)
        return false;

    FLOAT var_thre = 0.4; // TO-DO

    // 2D only now
    for (int k=0;k<leng;k++){ // look at Quatree.cpp ln.486
        EVectorX xt(2);
        xt << x[2*k], x[2*k+1];

        int k6 = 6*k;

        // query Cs
        AABB searchbb(xt(0),xt(1), setting.map_scale_param*4.0);
        std::vector<QuadTree*> quads;
        std::vector<FLOAT>    sqdst;
        t->QueryNonEmptyLevelC(searchbb,quads,sqdst);

        res[k6+3] = 1.0 + setting.map_noise_param ; // variance of sdf value
        if (quads.size() == 1){
            Point<FLOAT> ct = quads[0]->getCenter();
            std::shared_ptr<OnGPIS> gp = quads[0]->getGP();
            if (gp != nullptr){
                gp->test2Dpoint(xt,res[k6],res[k6+1],res[k6+2],res[k6+3],res[k6+4],res[k6+5]);
            }
        }
        else if (sqdst.size() > 1){
            // sort by distance
            std::vector<int> idx(sqdst.size());
            std::size_t n(0);
            std::generate(std::begin(idx), std::end(idx), [&]{ return n++; });
            std::sort(  std::begin(idx), std::end(idx),[&](int i1, int i2) { return sqdst[i1] < sqdst[i2]; } );

//             // get THE FIRST gp pointer
             std::shared_ptr<OnGPIS> gp = quads[idx[0]]->getGP();
            if (gp != nullptr){
                gp->test2Dpoint(xt,res[k6],res[k6+1],res[k6+2],res[k6+3],res[k6+4],res[k6+5]);
            }

            if (res[k6+3] > var_thre){
                FLOAT f2[4];
                FLOAT grad2[4*2];
                FLOAT var2[4*3];

                var2[0] = res[k6+3];
                int numc = sqdst.size();
                if (numc > 3) numc = 3;
                bool need_wsum = true;
                for (int m=0; m<(numc-1);m++){
                    int m_1 = m+1;
                    int m2 = m_1*2;
                    int m3 = m_1*3;
                    gp = quads[idx[m_1]]->getGP();
                    gp->test2Dpoint(xt,f2[m_1],grad2[m2],grad2[m2+1],var2[m3],var2[m3+1],var2[m3+2]);

                }

                if (need_wsum){
                    f2[0] = res[k6];
                    grad2[0] = res[k6+1];
                    grad2[1] = res[k6+2];
                    var2[1] = res[k6+4];
                    var2[2] = res[k6+5];
                    std::vector<int> idx(numc);
                    std::size_t n(0);
                    std::generate(std::begin(idx), std::end(idx), [&]{ return n++; });
                    std::sort(  std::begin(idx), std::end(idx),[&](int i1, int i2) { return var2[i1*3] < var2[i2*3]; } );

                    if (var2[idx[0]*3] < var_thre)
                    {
                        res[k6] = f2[idx[0]];
                        res[k6+1] = grad2[idx[0]*2];
                        res[k6+2] = grad2[idx[0]*2+1];
                        res[k6+3] = var2[idx[0]*3];
                        res[k6+4] = var2[idx[0]*3+1];
                        res[k6+5] = var2[idx[0]*3+2];
                    }
                    else{
                        FLOAT w1 = (var2[idx[0]*3] - var_thre);
                        FLOAT w2 = (var2[idx[1]*3] - var_thre);

                        FLOAT w12 = w1+w2;

                        res[k6] = (w2*f2[idx[0]]+w1*f2[idx[1]])/w12;
                        res[k6+1] = (w2*grad2[idx[0]*2]+w1*grad2[idx[1]*2])/w12;
                        res[k6+2] = (w2*grad2[idx[0]*2+1]+w1*grad2[idx[1]*2+1])/w12;
                        res[k6+3] = (w2*var2[idx[0]*3]+w1*var2[idx[1]*3])/w12;
                        res[k6+4] = (w2*var2[idx[0]*3+1]+w1*var2[idx[1]*3+1])/w12;
                        res[k6+5] = (w2*var2[idx[0]*3+2]+w1*var2[idx[1]*3+2])/w12;
                    }

                }
            }
        }
    }

    return true;
}

void GPisMap::test_kernel(int thread_idx,
                          int start_idx,
                          int end_idx,
                          FLOAT *x,
                          FLOAT *res){

    FLOAT var_thre = 0.4; // TO-DO

    for (int i = start_idx; i < end_idx; ++i){
        EVectorX xt(2);
        xt << x[2*i], x[2*i+1];

        int k6 = 6*i;

        // query Cs
        AABB searchbb(xt(0),xt(1), setting.map_scale_param*4.0);
        std::vector<QuadTree*> quads;
        std::vector<FLOAT>    sqdst;
        t->QueryNonEmptyLevelC(searchbb,quads,sqdst);

        res[k6+3] = 1.0 + setting.map_noise_param ; // variance of sdf value
        if (quads.size() == 1){
            Point<FLOAT> ct = quads[0]->getCenter();
            std::shared_ptr<OnGPIS> gp = quads[0]->getGP();
            if (gp != nullptr){
                gp->test2Dpoint(xt,res[k6],res[k6+1],res[k6+2],res[k6+3],res[k6+4],res[k6+5]);
            }
        }
        else if (sqdst.size() > 1){
            // sort by distance
            std::vector<int> idx(sqdst.size());
            std::size_t n(0);
            std::generate(std::begin(idx), std::end(idx), [&]{ return n++; });
            std::sort(  std::begin(idx), std::end(idx),[&](int i1, int i2) { return sqdst[i1] < sqdst[i2]; } );

            // get THE FIRST gp pointer
            std::shared_ptr<OnGPIS> gp = quads[idx[0]]->getGP();
            if (gp != nullptr){
                gp->test2Dpoint(xt,res[k6],res[k6+1],res[k6+2],res[k6+3],res[k6+4],res[k6+5]);
            }

            if (res[k6+3] > var_thre){
                FLOAT f2[4];
                FLOAT grad2[4*2];
                FLOAT var2[4*3];

                var2[0] = res[k6+3];
                int numc = sqdst.size();
                if (numc > 3) numc = 3;
                bool need_wsum = true;
                for (int m=0; m<(numc-1);m++){
                    int m_1 = m+1;
                    int m2 = m_1*2;
                    int m3 = m_1*3;
                    gp = quads[idx[m_1]]->getGP();
                    gp->test2Dpoint(xt,f2[m_1],grad2[m2],grad2[m2+1],var2[m3],var2[m3+1],var2[m3+2]);

                }

                if (need_wsum){
                    f2[0] = res[k6];
                    grad2[0] = res[k6+1];
                    grad2[1] = res[k6+2];
                    var2[1] = res[k6+4];
                    var2[2] = res[k6+5];
                    std::vector<int> idx(numc);
                    std::size_t n(0);
                    std::generate(std::begin(idx), std::end(idx), [&]{ return n++; });
                    std::sort(  std::begin(idx), std::end(idx),[&](int i1, int i2) { return var2[i1*3] < var2[i2*3]; } );

                    if (var2[idx[0]*3] < var_thre)
                    {
                        res[k6] = f2[idx[0]];
                        res[k6+1] = grad2[idx[0]*2];
                        res[k6+2] = grad2[idx[0]*2+1];
                        res[k6+3] = var2[idx[0]*3];
                        res[k6+4] = var2[idx[0]*3+1];
                        res[k6+5] = var2[idx[0]*3+2];
                    }
                    else{
                        FLOAT w1 = (var2[idx[0]*3] - var_thre);
                        FLOAT w2 = (var2[idx[1]*3] - var_thre);

                        FLOAT w12 = w1+w2;

                        res[k6] = (w2*f2[idx[0]]+w1*f2[idx[1]])/w12;
                        res[k6+1] = (w2*grad2[idx[0]*2]+w1*grad2[idx[1]*2])/w12;
                        res[k6+2] = (w2*grad2[idx[0]*2+1]+w1*grad2[idx[1]*2+1])/w12;
                        res[k6+3] = (w2*var2[idx[0]*3]+w1*var2[idx[1]*3])/w12;
                        res[k6+4] = (w2*var2[idx[0]*3+1]+w1*var2[idx[1]*3+1])/w12;
                        res[k6+5] = (w2*var2[idx[0]*3+2]+w1*var2[idx[1]*3+2])/w12;
                    }

                }
            }
        }

    }
}

bool GPisMap::test_mt(FLOAT * x, int dim, int leng, FLOAT * res){
    if (x == 0 || dim != mapDimension || leng < 1)
        return false;

    int num_threads = std::thread::hardware_concurrency();
    int num_threads_to_use = num_threads;
    if (leng < num_threads){
        num_threads_to_use = leng;
    }
    else{
        num_threads_to_use = num_threads;
    }
    std::thread *threads = new std::thread[num_threads_to_use];

    int num_leftovers = leng % num_threads_to_use;
    int batch_size = leng / num_threads_to_use;
    int element_cursor = 0;

    for(int i = 0; i < num_leftovers; ++i){
        threads[i] = std::thread(&GPisMap::test_kernel,
                                 this,
                                 i,
                                 element_cursor,
                                 element_cursor + batch_size + 1,
                                 x, res);
        element_cursor += batch_size + 1;

    }
    for (int i = num_leftovers; i < num_threads_to_use; ++i){
        threads[i] = std::thread(&GPisMap::test_kernel,
                                 this,
                                 i,
                                 element_cursor,
                                 element_cursor + batch_size,
                                 x, res);
        element_cursor += batch_size;
    }

    for (int i = 0; i < num_threads_to_use; ++i){
        threads[i].join();
    }

    delete [] threads;

    return true;
}

void GPisMap::getAllPoints(std::vector<FLOAT> & pos)
{
    pos.clear();

    if (t==0)
        return;

    std::vector<std::shared_ptr<Node> > nodes;
    t->getAllChildrenNonEmptyNodes(nodes);

    int N = nodes.size();
    if (N> 0){
        pos.resize(mapDimension*N);
        for (int j=0; j<N; j++) {
            int j2 = 2*j;
            pos[j2] = nodes[j]->getPosX();
            pos[j2+1] = nodes[j]->getPosY();
        }
    }
    return;
}

void GPisMap::getAllPoints(std::vector<FLOAT> & pos, std::vector<FLOAT> &var, std::vector<FLOAT> &grad,  std::vector<FLOAT> &grad_var)
{
    pos.clear();
    var.clear();
    grad.clear();
    grad_var.clear();

    if (t==0)
        return;
    std::vector<std::shared_ptr<Node> > nodes;
    t->getAllChildrenNonEmptyNodes(nodes);

    int N = nodes.size();
    if (N> 0){
        pos.resize(mapDimension*N);
        var.resize(N);
        grad.resize(mapDimension*N);
        grad_var.resize(N);
        for (int j=0; j<N; j++) {
            int j2 = 2*j;
            pos[j2] = nodes[j]->getPosX();
            pos[j2+1] = nodes[j]->getPosY();
            var[j] = nodes[j]->getPosNoise();
            grad[j2] = nodes[j]->getGradX();
            grad[j2+1] = nodes[j]->getGradY();
            grad_var[j] = nodes[j]->getGradNoise();
        }
    }
    return;
}
