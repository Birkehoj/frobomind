#include "roboteq_hbl1650/hbl1650.hpp"

Channel::Channel( ) : velocity_filter(8) // do not change!!
{
	down_time = 0;
	current_setpoint = 0;
	hall_value = last_hall = 0;
}

void Channel::onHallFeedback(ros::Time time, int feedback)
{
	message.hall.header.stamp = time;
	message.hall.data = feedback;

	//make hall values relative
	hall_value += feedback - last_hall;
	last_hall = feedback;

	publisher.hall.publish(message.hall);
	feedback_filter.push(message.hall);
}

void Channel::onPowerFeedback(ros::Time time, int feedback)
{
	message.power.header.stamp = time;
	message.power.data = feedback;
	publisher.power.publish(message.power);
}

void Channel::onTemperatureFeedback(ros::Time time, int feedback)
{
	message.temperature.header.stamp = time;
	message.temperature.data = feedback;
	publisher.temperature.publish(message.temperature);
}

void Channel::onCmdVel(const geometry_msgs::TwistStamped::ConstPtr& msg)
{
	velocity = msg->twist.linear.x;
	// limit velocity
	if(velocity >  max_velocity_mps) velocity = max_velocity_mps;
	if(velocity < -max_velocity_mps) velocity = -max_velocity_mps;

	time_stamp.last_twist_received = ros::Time::now();
}

void Channel::onDeadman(const std_msgs::Bool::ConstPtr& msg)
{
	if(msg->data)
		time_stamp.last_deadman_received = ros::Time::now();
}

void Channel::onTimer(const ros::TimerEvent& e, RoboTeQ::status_t& status)
{
	/* Register time */
	ros::Time now = ros::Time::now();
	std::stringstream ss, out; /* streams for holding status message and command output */
	static double test = 0;
	double period = 0, feedback = 0, feedback_filtered = 0, current_velocity = feedback_filter.get()*ticks_to_meter;

	if(status.online) /* is set when controller answers to FID request */
	{
		ss << "controller_online ";
		if(status.initialised) /* is set when init function completes */
		{
			ss << "controller_initialised ";
			if(status.responding) /* is set if the controller publishes serial messages */
			{
				ss << "controller_responding ";
				if(status.cmd_vel_publishing) /* is set if someone publishes twist messages */
				{
					ss << "cmd_vel_publishing ";
					if(status.deadman_pressed) /* is set if someone publishes true on deadman topic */
					{
						ss << "deadman_pressed ";

						if(status.emergency_stop)
						{
							/* release emergency stop */
							transmit("!MG");
							status.emergency_stop = false;
							current_setpoint = 0;
						}


						/* Calculate period */
						period = (now - time_stamp.last_regulation).toSec();

						/* Get latest feedback and reset */
						feedback = (( (double)hall_value)*ticks_to_meter)/period;
						hall_value = 0;

						/* Filter feedback */
						feedback_filtered = velocity_filter.update(feedback);

						if(position_control)
						{
							/* Position control with s-ramps */
							position_generator.setPeriod(period); //in seconds
							position_generator.setCurrentPosition(((double)last_hall)*ticks_to_meter); //in meters
							position_generator.setCurrentVelocityInput(velocity); //in meters per second
							desired_position = position_generator.getNewPosition(); //in meters

							/* Force 1m/s */
							desired_position = (((double)last_hall)*ticks_to_meter) + 0.25*period;
							current_setpoint = regulator.output_from_input(desired_position*mps_to_thrust, ((double)last_hall)*ticks_to_meter*mps_to_thrust, period);
							//current_setpoint = regulator.output_from_input(desired_position, ((double)last_hall)*ticks_to_meter, period); //setpoint in meters per second from inputs in meters, meters and seconds

						}
						else
						{
							/* Velocity control with feed forward */
							current_setpoint = velocity + regulator.output_from_input(velocity, current_velocity , period);
						}

						current_thrust =  (int)(current_setpoint * mps_to_thrust); //Thrust in RoboTeQ units

						/* Implement maximum output*/
						if(current_thrust >  max_output) current_thrust = max_output;
						if(current_thrust < -max_output) current_thrust = -max_output;


						/* Send motor output command  */
						out << "!G " << ch << " " << current_thrust;
						transmit(out.str());

						// Upkeep
						time_stamp.last_regulation = now;
					}
					else /* deadman not pressed */
					{
						/* Set speeds to 0 and activate emergency stop */
						transmit("!EX");
						status.emergency_stop = true;
						transmit("!G 1 0");
						transmit("!G 2 0");
						current_setpoint = current_thrust = 0;
						velocity = 0;
						regulator.reset_integrator();
					}
				}
				else /* Cmd_vel is not publishing */
				{
					/* Set speeds to 0 and activate emergency stop */
					transmit("!EX");
					status.emergency_stop = true;
					transmit("!G 1 0");
					transmit("!G 2 0");
					current_setpoint = current_thrust = 0;
					velocity = 0;
					regulator.reset_integrator();

				}
			}
			else /* controller is not responding */
			{
				ROS_INFO_THROTTLE(5,"%s: Controller is not responding",ros::this_node::getName().c_str());
				down_time++;
				if(down_time > 10)
				{
					/* Try to re-connect and re-initialise */
					transmit("?FID");
					down_time = 0;
				}
			}
		}
		else /* Controller is not initialised */
		{
			ROS_INFO("%s: Controller is not initialised",ros::this_node::getName().c_str());
			//initController("standard");
			//status.initialised = true;'
			transmit("?FID");
		}
	}
	else /* controller is not online */
	{
		ROS_INFO_THROTTLE(5,"%s: Controller is not yet online",ros::this_node::getName().c_str());
		/* Try to re-connect and re-initialise */
		transmit("?FID");
	}
	/* Publish feedback */
	message.feedback.header.stamp = now;
	message.feedback.velocity = current_velocity; /* Velocity in m/s */
	message.feedback.velocity_setpoint = velocity;
	message.feedback.thrust = (current_thrust*100)/roboteq_max; /* Thrust in % */
	publisher.feedback.publish(message.feedback);

	/* Publish the status message */
	message.status.header.stamp = ros::Time::now();
	message.status.data = ss.str();
	publisher.status.publish(message.status);

//	std::cout << "Period[s]:" << period <<
//			" Setpoint[m/s]:" << velocity <<
//			" Current position[m]:" << ((double)last_hall)*ticks_to_meter <<
//			" Desired position[m]:" << desired_position <<
//			" Thrust[]:" << current_thrust <<
//			" " << std::endl;
}

