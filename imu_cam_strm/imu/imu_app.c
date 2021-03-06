#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <fcntl.h>
#include <math.h>
#include <util/util.h>
#include <errno.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <time.h>

#define SLAVE_ADDR 0x68
#define IMU_SAMPLE_RATE_HZ 100

#define PRINT_QUAT      (0x04)

#define COMM_PORT_NUM 9930
#define COMM_SRV_IP "148.220.23.16"
#define SYSFS_ADC_DIR "/sys/bus/iio/devices/iio:device0/in_voltage0_raw"
#define MAX_BUFF 64


int mi_atoi(const char *cad, int tam);
int caracter_valido(const char caracter);
int readADC(unsigned int pin);
void reverse(char s[]);
void itoa(int n, char s[]);


static I32 file;
I8 *filename = "/dev/i2c-1";
double boot_ts_us;

static signed char gyro_orientation[9] = {-1, 0, 0,
	0,-1, 0,
	0, 0, 1};

struct rx_s {
	unsigned char header[3];
	unsigned char cmd;
};

struct hal_s {
	unsigned char sensors;
	unsigned char dmp_on;
	unsigned char wait_for_tap;
	volatile unsigned char new_gyro;
	unsigned short report;
	unsigned short dmp_features;
	unsigned char motion_int_mode;
	struct rx_s rx;
};

typedef struct euler_t {
	F32 roll;
	F32 pitch;
	F32 yaw;
} euler;

static struct hal_s hal = {0};

I32 s1;

void sig_handler(I32 signo)
{
	if (signo == SIGINT)
		printf("\nreceived SIGINT\n");
	exit(1);
}

void delay_ms(unsigned long num_ms) {
	usleep(num_ms*1000);
}

I32 min(I32 a, I32 b) {
	if (a <= b)
		return a;
	else
		return b;
}

void get_ms(unsigned long *count) {
	struct timeval ts;
	double total_time;
	unsigned long ms_count;

	gettimeofday(&ts, NULL);

	total_time = (double)(ts.tv_sec * 1e6) + (double)ts.tv_usec;
	total_time -= boot_ts_us;    
	ms_count = total_time/1000;

	*count = ms_count;
}

I32 i2c_write(U8 addr, U8 reg, U8 nBytes, U8* data_ptr) 
{
	I32 status;
	U8 buf[nBytes+1];
	U8 i; 

	ASSERT(nBytes >= 1);
	ASSERT(addr == SLAVE_ADDR);
	ASSERT(reg <= 0x75);

	buf[0] = reg;
	for(i=1; i<= nBytes; i++) {
		buf[i] = *data_ptr;
		data_ptr++;
	} 

	status = write(file, buf, i);
	if (status == i)
		return 0;
	else
		return -1;
}

I32 i2c_read(U8 addr, U8 reg, U8 nBytes, U8* data_ptr) 
{
	I32 status;

	ASSERT(nBytes >= 1);
	ASSERT(addr == SLAVE_ADDR);
	ASSERT(reg <= 0x75);

	status = write(file, &reg, 1);
	if (status != 1)
		return -1;

	status = read(file, data_ptr, nBytes); 
	if (status == nBytes)
		return 0;
	else
		return -1;
}

U8 read_imu_reg(U8 reg) {

	I32 status;
	U8 data;
	U8 buf; 

	/*Max IMU reg number check*/
	ASSERT(reg <= 0x75);   

	/*Write reg number over I2C to IMU*/
	buf = reg;
	status = write(file, &buf, 1);
	ASSERT(status == 1);

	/*Read over I2C from IMU*/
	status = read(file, &data, 1);
	ASSERT(status == 1);

	return data;
}

U8 write_imu_reg(U8 reg, U8 val) {

	U8 status;
	U8 buf[2];

	ASSERT(reg <= 0x75);
	ASSERT(val <= 0xFF);

	buf[0] = reg;
	buf[1] = val;

	status = write(file, buf, 2);

	return status;
}

#define EPSILON		0.0001f
#define PI		3.14159265358979323846f
#define PI_2		1.57079632679489661923f

static void quaternionToEuler( const float* quat_wxyz, float* x, float* y, float* z )
{
	float test;
	const struct quat { float w, x, y, z; } *q = ( const struct quat* )quat_wxyz;

	float sqy = q->y * q->y;
	float sqz = q->z * q->z;
	float sqw = q->w * q->w;

	test = q->x * q->z - q->w * q->y;

	if( test > 0.5f - EPSILON )
	{
		*x = 2.f * atan2( q->y, q->w );
		*y = PI_2;
		*z = 0;
	}
	else if( test < -0.5f + EPSILON )
	{
		*x = -2.f * atan2( q->y, q->w );
		*y = -PI_2;
		*z = 0;
	}
	else
	{
		*x = atan2( 2.f * ( q->x * q->w + q->y * q->z ), 1.f - 2.f * ( sqz + sqw ) );
		*y = asin( 2.f * test );
		*z = atan2( 2.f * ( q->x * q->y - q->z * q->w ), 1.f - 2.f * ( sqy + sqz ) );
	}
}


U8 dmpGetGravity(F32 *v, long *q) {
	v[0] = 2 * (q[1]*q[3] - q[0]*q[2]);
	v[1] = 2 * (q[0]*q[1] + q[2]*q[3]);
	v[2] = q[0]*q[0] - q[1]*q[1] - q[2]*q[2] + q[3]*q[3];
	return 0;
}

U8 dmpGetEuler(float *data, long *q) {
	data[0] = atan2(2*q[1]*q[2] - 2*q[0]*q[3], 2*q[0]*q[0] + 2*q[1]*q[1] - 1);   // psi
	data[1] = -asin(2*q[1]*q[3] + 2*q[0]*q[2]);                              // theta
	data[2] = atan2(2*q[2]*q[3] - 2*q[0]*q[1], 2*q[0]*q[0] + 2*q[3]*q[3] - 1);   // phi
	return 0;
}

U8 dmpGetYawPitchRoll(float *data, long *q, F32 *gravity) {
	// yaw: (about Z axis)
	data[0] = 2.0*atan2(2*q[1]*q[2] - 2*q[0]*q[3], 2*q[0]*q[0] + 2*q[1]*q[1] - 1);

	// pitch: (nose up/down, about Y axis)
	if (gravity[2] > 0) 
		data[1] = atan(gravity[0] / sqrt(gravity[1]*gravity[1] + gravity[2]*gravity[2]));
	else 
		data[1] = -1.0*PI - atan(gravity[0] / sqrt(gravity[1]*gravity[1] + gravity[2]*gravity[2]));
	if (data[1] > 0)
		data[1] = -2.0*PI + data[1];
	if (data[1] < 0)
		data[1] *= -1.0;

	// roll: (tilt left/right, about X axis)
	if (gravity[2] > 0)
		data[2] = atan(gravity[1] / sqrt(gravity[0]*gravity[0] + gravity[2]*gravity[2]));
	else 
		data[2] = -1.0*PI - atan(gravity[1] / sqrt(gravity[0]*gravity[0] + gravity[2]*gravity[2]));
	if (data[2] > 0)
		data[2] = -2.0*PI + data[2];
	if (data[2] < 0)
		data[2] *= -1.0;

	return 0;
}

int main(void) {

	int fd,k,i;
	float x,j=0,tl=0.005;
	char buf[MAX_BUFF];
	char ch[5];
	char ch2[5];
	ch[4] = 0;
	I32 status;
	struct timeval boot_ts;
	static I32 gpio_file;
	fd_set read_fd;
	struct timeval ts;
	U8 val;
	euler angle;
	F32 gravity[3];
	F32 angle_data[3];

	U8 sndBuf[64];
	I32 sndBuf_idx;
	I32 data_i32;
	F32 data_f32;
	U32 data_u32;

	if (signal(SIGINT, sig_handler) == SIG_ERR) {
		printf("\ncan't catch SIG Handler\n");
		exit(1);
	}

	gettimeofday(&boot_ts, NULL);
	boot_ts_us = (double)(boot_ts.tv_sec * 1e6) + (double)boot_ts.tv_usec;

	status = file = open(filename, O_RDWR);
	if (status < 0) {
		/* ERROR HANDLING: you can check errno to see what went wrong */
		perror("Failed to open the i2c bus");
		ASSERT(status >= 0)
	}

	status = ioctl(file, I2C_TENBIT, 0);
	if (status < 0) {
		printf("Failed to set TENBIT addressing to 0\n");
		/* ERROR HANDLING; you can check errno to see what went wrong */
		ASSERT(status >= 0);
	}

	I32 addr = SLAVE_ADDR;         
	status = ioctl(file, I2C_SLAVE, addr);
	if (status < 0) {
		printf("Failed to acquire bus access and/or talk to slave.\n");
		/* ERROR HANDLING; you can check errno to see what went wrong */
		ASSERT(status >= 0);
	}

	if( access("/sys/class/gpio/gpio40/value", F_OK ) != -1 ) {
		// file exists
		gpio_file = open("/sys/class/gpio/gpio40/value", O_RDONLY);
		if (gpio_file < 0) {
			/* ERROR HANDLING: you can check errno to see what went wrong */
			perror("Failed to open the gpio file\n");
			ASSERT(gpio_file >= 0)
		}
	} else {
		// file doesn't exist
		printf("GPIO_40 file doesn't exist. Execute \'echo $GPIO > export\' \
				in /sys/class/gpio as root where $GPIO = 40\n");
		exit(1);
	}


	s1 = sock_init(COMM_PORT_NUM, COMM_SRV_IP);
	ASSERT(s1 != -1);
	printf("IMU COMM socket successfully initialized\n");

	FILE *fw=NULL;
	fw=fopen("/mnt/remoteserver/lectura.csv","a");
	// fw=fopen("lectura.txt","a");
	while(1) {

		//crea el archivo y lo abre para escribir
		for(i=0;i<=20;i++)
		{
			//habilita la lectura del adc usando: echo cape-bone-iio > /sys/devices/bone_capemgr.*/slots
			snprintf(buf, sizeof(buf), SYSFS_ADC_DIR);
			fd=open(buf, O_RDONLY);
			read(fd,ch,4);
			x=mi_atoi(ch,strlen(ch));
			x=(x*100)/4095;
			printf("%f\n",x);
			close(fd);

			//escribe en archivo

			fprintf(fw,"%f,%f\n",x,j);

			sleep(tl);
			j+=tl;
		}
	}
	fclose(fw);

	return 0;
}

int caracter_valido(const char cad)
{
	return cad >= '0' && cad <='9';
}

//Convierte una cadena de caracteres a enteros
int mi_atoi(const char *cad, int tam)
{
	int i=tam-1, entero=0, pos=1;
	while (i>=0)
	{
		while (caracter_valido(cad[i]))
		{
			entero=entero+(cad[i]-48)*pos;
			pos=pos*10;
			i--;
		}
		pos =1;
		i--;
	}
	return entero;
}
/* itoa:  convert n to characters in s */
void itoa(int n, char s[])
{
	int i, sign;

	if ((sign = n) < 0)  /* record sign */
		n = -n;          /* make n positive */
	i = 0;
	do {       /* generate digits in reverse order */
		s[i++] = n % 10 + '0';   /* get next digit */
	} while ((n /= 10) > 0);     /* delete it */
	if (sign < 0)
		s[i++] = '-';
	s[i] = '\0';
	reverse(s);
}
/* reverse:  reverse string s in place */
void reverse(char s[])
{
	int i, j;
	char c;

	for (i = 0, j = strlen(s)-1; i<j; i++, j--) {
		c = s[i];
		s[i] = s[j];
		s[j] = c;
	}
}
