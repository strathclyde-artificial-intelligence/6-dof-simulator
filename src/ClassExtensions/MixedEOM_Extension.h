#ifndef __QUADROTOREOM_EXTENSION_H__
#define __QUADROTOREOM_EXTENSION_H__

#include <EquationsOfMotion/MixedEOM.h>
#include "../Containers/DroneConfig.h"

class MixedEOM : public caelus_fdm::MixedEOM {
public:
    MixedEOM(DroneConfig conf) : caelus_fdm::MixedEOM(
        conf.b_prop,
        conf.c,
        conf.b_aero,
        conf.S,
        conf.d,
        conf.l,
        conf.drone_aero_config,
        conf.J,
        NULL,
        NULL,
        conf.mass,
        9.81
    ) {}

    bool get_airborne_status() {
        return this->is_airborne;
    }

//         int evaluate(const double &t, const caelus_fdm::State &x, caelus_fdm::StateDerivative &dx) override {
//             // body frame linear and angular velocity
//             Eigen::Vector3d Vb = x.segment(3,3);
//             Eigen::Vector3d wb = x.segment(9,3);
            
//             if (this->is_airborne) {
//                 m_weightFM.updateParamsImpl(t,x);
//                 m_thrustFixedWingFM.updateParamsImpl(t,x);
                
//             }
//             m_AeroFM.updateParamsImpl(t,x);
//             m_thrustQuadFM.updateParamsImpl(t, x);

//             m_fg = this->is_airborne ? m_weightFM.getF() : Eigen::VectorXd::Zero(3);
//             m_fa = m_AeroFM.getF();
//             m_fa = caelus_fdm::wind2body(x) * m_fa;
            
//             m_ft = this->is_airborne ? m_thrustFixedWingFM.getF() : Eigen::VectorXd::Zero(3);
//             m_fq = m_thrustQuadFM.getF();

//             m_mg = this->is_airborne ? m_weightFM.getM() : Eigen::VectorXd::Zero(3);
//             m_ma = m_AeroFM.getM();
//             m_mt = this->is_airborne ? m_thrustFixedWingFM.getM() : Eigen::VectorXd::Zero(3);
//             m_mq = m_thrustQuadFM.getM();

// //            m_ft[0] = -m_fa[0]*m_thrustFixedWingFM.get_controller_prop(t)[0];

//             // assign output
//             dx = caelus_fdm::StateDerivative(x.size());

//             // earth-frame velocity
//             dx.segment(0,3) = caelus_fdm::body2earth(x)*Vb;

//             // body-frame acceleration
//             dx.segment(3,3)  = (m_ft+m_fg+m_fa+m_fq)/m_weightFM.get_mass(); // external forces
            
//             // earth-frame angle rates
//             dx.segment(6,3) = caelus_fdm::angularVelocity2eulerRate(x)*wb;

//             // body-frame angular acceleration
//             dx.segment(9,3)  = m_mt+m_ma+m_mq; // external torques
//             dx.segment(9,3) -= wb.cross( m_I*wb.eval() ); // account for frame dependent ang acc
//             dx.segment(9,3)  = m_I.colPivHouseholderQr().solve(dx.segment(9,3).eval()); // inertia matrix into account
            
//             this->is_airborne = fabs(m_fq[2]) >= fabs(m_weightFM.getF()[2]) ? true : false;

//             return 0;
//         }
};

#endif // __MIXEDEOM_EXTENSION_H__